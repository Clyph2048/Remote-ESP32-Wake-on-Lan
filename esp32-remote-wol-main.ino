#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include "esp_system.h"   // CHANGE: esp_reset_reason()
#include "esp_timer.h"    // CHANGE: esp_timer_get_time() for a 64-bit uptime (millis() wraps at 49.7 days)
#include "mbedtls/md.h"
#include "secrets.h"      // Credentials stay here: SECRET_WIFI_NAME, SECRET_WIFI_PASSWORD,
                          // SECRET_MQTT_USER, SECRET_MQTT_PASS, SECRET_PC_IP

// #define LED_BUILTIN 2
#define USE_HASHED_ID 1

// Nicholas Persson 2026
// Remote Wake on Lan for ESP32
// Based on the work of https://github.com/kreaxv/esp32-remote-wol
// May need some tweaks for individual use
//
// Robustness/diagnostics revision: see the CHANGE comments and the change list
// at the bottom of this file.

// ----------------------- Build options -----------------------
// CHANGE: fallback so the sketch still builds on boards whose variant has no LED_BUILTIN.
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// CHANGE: some boards (for example the Seeed XIAO ESP32-C6, user LED on GPIO15) wire the LED
// active-LOW. Set this to 1 on such boards so that "LED off = OK" really means off.
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 0
#endif

// CHANGE: publish the last reset reason once per boot to <user>/feeds/<DIAG_FEED_KEY>.
// That is 1 data point per boot, so it barely touches the 30/min budget. Adafruit IO creates the
// feed on first publish, but only if you are under the free tier's 10-feed cap. If you are at the
// cap, the publish is rejected and the reason arrives on <user>/errors, which this sketch now logs.
#define PUBLISH_RESET_REASON 1
static const char* DIAG_FEED_KEY = "esp32-wol-diag";

// CHANGE: the GPIO3/GPIO14 writes in setup() are kept behind this switch. See the REVIEW note in setup().
#define CONFIGURE_RF_SWITCH_PINS 1

// ----------------------- MQTT -----------------------
static const char* MQTT_HOST = "io.adafruit.com";
static const uint16_t MQTT_PORT = 8883;
static const char* MQTT_USER = SECRET_MQTT_USER;
static const char* MQTT_PASS = SECRET_MQTT_PASS;

// ----------------------- Recovery tuning -----------------------
// CHANGE: WiFi backoff 2 -> 4 -> 8 -> 16 s, restart if WiFi has been down > 60 s.
static const unsigned long WIFI_RETRY_MIN_MS  = 2000;
static const unsigned long WIFI_RETRY_MAX_MS  = 16000;
static const unsigned long WIFI_RESTART_MS    = 60000;
// CHANGE: MQTT backoff 2 -> 4 -> ... -> 60 s. Worst case is about 5 attempts in the first minute,
// then 1/min, far below Adafruit IO's 20 connection attempts/min.
static const unsigned long MQTT_RETRY_MIN_MS  = 2000;
static const unsigned long MQTT_RETRY_MAX_MS  = 60000;
// CHANGE: backoff only resets after the session has stayed up this long. A flapping connection
// (connects, then drops at once) therefore cannot hammer the broker.
static const unsigned long MQTT_STABLE_MS     = 60000;
static const unsigned long MQTT_RESTART_MS    = 10UL * 60UL * 1000UL;  // 10 min
// CHANGE: bound how long one blocking TLS/MQTT connect attempt can take.
static const uint16_t MQTT_SOCKET_TIMEOUT_S   = 10;
static const unsigned long TLS_HANDSHAKE_TIMEOUT_S = 10;
// CHANGE: explicit TCP timeout for the heartbeat probe.
static const int32_t PING_TIMEOUT_MS = 1000;

// ----------------------- Globals -----------------------
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);
WiFiUDP udp;

bool isPcOn = false;
bool isStartup = true;
const char* PC_IP_ADDRESS = SECRET_PC_IP;
const int MOONLIGHT_PORT = 47989;
unsigned long lastPingTime = 0;
const unsigned long PING_INTERVAL = 10000;

static uint8_t mac[6];
static char esp32mac[65];
static char topicCmd[80];
static char macColon[18];

// CHANGE: all topics are built once, not on every loop() pass.
static char topicStatus[80];
static char topicThrottle[80];   // <user>/throttle : Adafruit IO rate-limit warnings
static char topicErrors[80];     // <user>/errors   : Adafruit IO rejected-publish/auth errors
static char topicDiag[96];
static char clientId[24];        // CHANGE: stable, MAC-derived (MQTT 3.1 safe: <= 23 chars)

static IPAddress pcIp;           // CHANGE: parsed once; falls back to DNS if SECRET_PC_IP is a hostname
static bool pcIpIsLiteral = false;

// Diagnostics state. Written from the WiFi event task, so volatile.
static volatile uint8_t  lastWifiDisconnectReason = 0;
static volatile uint32_t wifiDisconnectCount = 0;
static uint32_t mqttConnectCount = 0;
static char resetReasonText[48] = "UNKNOWN";
static bool resetReasonPublished = false;

// CHANGE: survives ESP.restart() (not power loss), so after a watchdog-style restart
// the log says *why* this sketch restarted itself, not just "SOFTWARE".
#define RTC_CAUSE_MAGIC 0xC0FFEE42UL
enum RestartCause : uint32_t { CAUSE_NONE = 0, CAUSE_WIFI_TIMEOUT = 1, CAUSE_MQTT_TIMEOUT = 2 };
RTC_NOINIT_ATTR static uint32_t rtcCauseMagic;
RTC_NOINIT_ATTR static uint32_t rtcCause;

// ======================================================================
// LED state machine (CHANGE: replaces every blocking blinkLed() call)
// ======================================================================
// A pattern is a list of durations in ms, alternating ON, OFF, ON, OFF...
// Background patterns repeat (status). One-shot patterns play once on top, then the background resumes.
struct LedPattern { const uint16_t* steps; uint8_t len; bool repeat; };

static const uint16_t STEPS_WIFI_DOWN[] = {500, 500};                        // 1 Hz, as blinkLed(1,500)
static const uint16_t STEPS_MQTT_FAIL[] = {500, 500, 500, 2500};             // double blink + old 2 s pause
static const uint16_t STEPS_PC_OFF[]    = {500, 500, 500, 500, 500, 500};    // 3 slow, as blinkLed(3,500)
static const uint16_t STEPS_WOL[]       = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50}; // 5 fast, as blinkLed(5,50)

static const LedPattern LED_WIFI_DOWN = {STEPS_WIFI_DOWN, 2,  true};
static const LedPattern LED_MQTT_FAIL = {STEPS_MQTT_FAIL, 4,  true};
static const LedPattern LED_PC_OFF    = {STEPS_PC_OFF,    6,  false};
static const LedPattern LED_WOL       = {STEPS_WOL,       10, false};

static const LedPattern* ledBackground = nullptr;  // nullptr = solid off = OK
static const LedPattern* ledOneShot    = nullptr;
static const LedPattern* ledCurrent    = nullptr;
static uint8_t ledStep = 0;
static unsigned long ledStepStart = 0;

static void ledWrite(bool on) {
  digitalWrite(LED_BUILTIN, (on ^ (LED_ACTIVE_LOW != 0)) ? HIGH : LOW);
}

static void ledSetBackground(const LedPattern* p) { ledBackground = p; }

static void ledPlayOnce(const LedPattern* p) {
  ledOneShot = p;
  ledCurrent = nullptr;   // force a restart even if the same pattern is already playing
}

static void ledUpdate(unsigned long now) {
  const LedPattern* desired = ledOneShot ? ledOneShot : ledBackground;
  if (desired != ledCurrent) {
    ledCurrent = desired;
    ledStep = 0;
    ledStepStart = now;
    ledWrite(ledCurrent != nullptr);   // step 0 is always ON
    return;
  }
  if (!ledCurrent) return;
  if (now - ledStepStart < ledCurrent->steps[ledStep]) return;

  ledStepStart = now;
  if (++ledStep >= ledCurrent->len) {
    if (ledCurrent->repeat) {
      ledStep = 0;
    } else {               // one-shot finished: hand back to the background pattern
      ledOneShot = nullptr;
      ledWrite(false);
      return;
    }
  }
  ledWrite((ledStep % 2) == 0);
}

// --------------------- SHA-256 ---------------------
static void sha256Hex(const char* input, char* output) {
  uint8_t hash[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mbedtls_md(info, (const unsigned char*)input, strlen(input), hash) != 0) {
    output[0] = 0;
    return;
  }
  for (int i = 0; i < 32; i++) sprintf(output + i * 2, "%02x", hash[i]);
  output[64] = 0;
}

// --------------------- Hex Parsing ---------------------
static uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}

// ======================================================================
// Diagnostics helpers (CHANGE)
// ======================================================================
static const char* resetReasonStr(esp_reset_reason_t r) {
  switch ((int)r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXTERNAL_PIN";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    // ESP-IDF 5.x (core 3.x) adds USB/JTAG/EFUSE/PWR_GLITCH/CPU_LOCKUP. They aren't listed
    // here so the same source builds on 2.x; they print as UNKNOWN plus the numeric code.
    default:                return "UNKNOWN";
  }
}

// Common wifi_err_reason_t codes. Own table, because WiFi.disconnectReasonName() is not
// present in every 2.x release.
static const char* wifiReasonStr(uint8_t r) {
  switch (r) {
    case 1:   return "UNSPECIFIED";
    case 2:   return "AUTH_EXPIRE";
    case 3:   return "AUTH_LEAVE";
    case 4:   return "ASSOC_EXPIRE";
    case 5:   return "ASSOC_TOOMANY";
    case 8:   return "ASSOC_LEAVE";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT";
    case 16:  return "GROUP_KEY_UPDATE_TIMEOUT";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    case 206: return "AP_TSF_RESET";
    case 207: return "ROAMING";
    default:  return "OTHER";
  }
}

static const char* mqttStateStr(int s) {
  switch (s) {
    case -4: return "CONNECTION_TIMEOUT";
    case -3: return "CONNECTION_LOST";
    case -2: return "CONNECT_FAILED";
    case -1: return "DISCONNECTED";
    case 0:  return "CONNECTED";
    case 1:  return "BAD_PROTOCOL";
    case 2:  return "BAD_CLIENT_ID";
    case 3:  return "UNAVAILABLE";
    case 4:  return "BAD_CREDENTIALS";   // check SECRET_MQTT_USER / AIO key
    case 5:  return "UNAUTHORIZED";
    default: return "?";
  }
}

static void formatUptime(char* buf, size_t n) {
  uint64_t s = (uint64_t)(esp_timer_get_time() / 1000000ULL);
  snprintf(buf, n, "%lud%02lu:%02lu:%02lu",
           (unsigned long)(s / 86400), (unsigned long)((s / 3600) % 24),
           (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
}

// CHANGE: one line per heartbeat with everything needed to tell "weak signal" from
// "leaking heap" from "PC actually off".
static void logHealth(bool pcUp, unsigned long probeMs) {
  char up[24];
  formatUptime(up, sizeof(up));
  uint8_t lr = lastWifiDisconnectReason;
  Serial.printf("[HB] PC=%s probe=%lums RSSI=%ddBm heap=%u (min %u) up=%s "
                "wifiDisc=%lu last=%u(%s) mqttConn=%lu\n",
                pcUp ? "ON" : "OFF", probeMs, (int)WiFi.RSSI(),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), up,
                (unsigned long)wifiDisconnectCount, lr, lr ? wifiReasonStr(lr) : "-",
                (unsigned long)mqttConnectCount);
}

static void restartWithCause(RestartCause cause, const char* why) {
  Serial.printf("[SYS] Restarting: %s\n", why);
  Serial.flush();
  rtcCauseMagic = RTC_CAUSE_MAGIC;
  rtcCause = cause;
  delay(100);   // only blocking delay left in loop(); we are about to reboot anyway
  ESP.restart();
}

// CHANGE: WiFi event logging. ARDUINO_EVENT_* names are valid on core 2.x and 3.x
// (1.0.x used SYSTEM_EVENT_* and info.disconnected.reason).
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WiFi] Associated with AP");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] Got IP %s, RSSI %d dBm\n",
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      uint8_t reason = info.wifi_sta_disconnected.reason;
      lastWifiDisconnectReason = reason;
      wifiDisconnectCount = wifiDisconnectCount + 1;
      Serial.printf("[WiFi] Disconnected, reason %u (%s)\n", reason, wifiReasonStr(reason));
      break;
    }
    default:
      break;
  }
}

// --------------------- WOL ---------------------
// FIX: tolerant MAC parser. Scans the whole payload (not a 31-byte copy) for the first
// AA:BB:CC:DD:EE:FF, AA-BB-CC-DD-EE-FF, or AABBCCDDEEFF. Surrounding quotes, spaces,
// trailing \r\n, or a JSON wrapper no longer cause a rejection.
static bool isHexByte(byte c) { return isxdigit((unsigned char)c) != 0; }

static bool parseMac(const byte* p, unsigned int n, uint8_t out[6]) {
  for (unsigned int i = 0; i < n; i++) {
    // Separated form: 17 chars, same ':' or '-' separator throughout.
    if (i + 17 <= n && (p[i + 2] == ':' || p[i + 2] == '-')) {
      byte sep = p[i + 2];
      bool ok = true;
      for (int k = 0; k < 6 && ok; k++) {
        unsigned int j = i + k * 3;
        ok = isHexByte(p[j]) && isHexByte(p[j + 1]) && (k == 5 || p[j + 2] == sep);
      }
      if (ok) {
        for (int k = 0; k < 6; k++)
          out[k] = (hexNibble(p[i + k*3]) << 4) | hexNibble(p[i + k*3 + 1]);
        return true;
      }
    }
    // Bare form: exactly 12 hex digits, not part of a longer hex/number run.
    if (i + 12 <= n && (i == 0 || !isHexByte(p[i - 1])) &&
        (i + 12 == n || !isHexByte(p[i + 12]))) {
      bool ok = true;
      for (int k = 0; k < 12 && ok; k++) ok = isHexByte(p[i + k]);
      if (ok) {
        for (int k = 0; k < 6; k++)
          out[k] = (hexNibble(p[i + 2*k]) << 4) | hexNibble(p[i + 2*k + 1]);
        return true;
      }
    }
  }
  return false;
}

static void sendWOL(const uint8_t targetMac[6]) {
  udp.beginPacket(WiFi.broadcastIP(), 9);
  for (int i = 0; i < 6; i++) udp.write(0xFF);
  for (int i = 0; i < 16; i++) udp.write(targetMac, 6);
  udp.endPacket();

  Serial.printf("WOL Packet Sent to %02X:%02X:%02X:%02X:%02X:%02X\n",
                targetMac[0], targetMac[1], targetMac[2],
                targetMac[3], targetMac[4], targetMac[5]);
}

// FIX: shows invisible characters (\r=0D, \n=0A, quote=22, BOM=EF BB BF) in a payload.
static void dumpPayload(const char* topic, const byte* payload, unsigned int length) {
  Serial.printf("MQTT Message Received on '%s' (%u bytes): ", topic, length);
  Serial.write(payload, length);
  Serial.print("\n  hex:");
  for (unsigned int i = 0; i < length && i < 48; i++) Serial.printf(" %02X", payload[i]);
  Serial.println(length > 48 ? " ..." : "");
}

static bool endsWith(const char* s, const char* suffix) {
  size_t ls = strlen(s), lx = strlen(suffix);
  return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

// --------------------- MQTT ---------------------
static void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  // CHANGE: Adafruit IO service topics are printed in full and handled before the
  // 32-byte command buffer, because their messages are longer than a MAC.
  if (strcmp(topic, topicThrottle) == 0 || strcmp(topic, topicErrors) == 0) {
    Serial.print(strcmp(topic, topicThrottle) == 0 ? "[AIO THROTTLE] " : "[AIO ERROR] ");
    Serial.write(payload, length);
    Serial.println();
    return;
  }

  dumpPayload(topic, payload, length);   // FIX: log exact topic + raw bytes

  // FIX: back to the original suffix match. The exact strcmp() fails if the broker's topic
  // differs from ours in username case or feed-key form (for example a group prefix).
  if (endsWith(topic, "wake-on-lan")) {
    uint8_t target[6];
    if (parseMac(payload, length, target)) {
      Serial.println("WOL Trigger Received");
      sendWOL(target);            // send first; the blink follows, non-blocking
      ledPlayOnce(&LED_WOL);
    } else {
      Serial.println("Invalid MAC payload rejected (no MAC found; see hex dump above).");
    }
  } else {
    Serial.printf("Topic did not match; expected one ending in 'wake-on-lan' (subscribed: %s)\n",
                  topicCmd);
  }
}

// ======================================================================
// WiFi recovery (CHANGE)
// ======================================================================
static bool wifiDown = false;
static unsigned long wifiDownSince = 0;
static unsigned long wifiLastRetry = 0;
static unsigned long wifiRetryDelay = WIFI_RETRY_MIN_MS;

// Returns true when WiFi is up. Never blocks.
static bool handleWiFi(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiDown) {
      Serial.printf("[WiFi] Recovered after %lus\n", (now - wifiDownSince) / 1000);
      wifiDown = false;
    }
    wifiRetryDelay = WIFI_RETRY_MIN_MS;
    return true;
  }

  if (!wifiDown) {
    wifiDown = true;
    wifiDownSince = now;
    wifiLastRetry = now;
    Serial.println("ERROR - WIFI DISCONNECTED");
  }

  // Core auto-reconnect stays on. On core 2.x it does not retry for every disconnect reason,
  // so an explicit reconnect() with backoff covers the gaps.
  if (now - wifiLastRetry >= wifiRetryDelay) {
    wifiLastRetry = now;
    Serial.printf("[WiFi] reconnect() (down %lus, status %d, next retry in %lus)\n",
                  (now - wifiDownSince) / 1000, (int)WiFi.status(),
                  min(wifiRetryDelay * 2, WIFI_RETRY_MAX_MS) / 1000);
    WiFi.reconnect();
    wifiRetryDelay = min(wifiRetryDelay * 2, WIFI_RETRY_MAX_MS);
  }

  if (now - wifiDownSince >= WIFI_RESTART_MS) {
    restartWithCause(CAUSE_WIFI_TIMEOUT, "WiFi down > 60 s");
  }
  return false;
}

// ======================================================================
// MQTT recovery (CHANGE: replaces the blocking connectMQTT() retry)
// ======================================================================
static bool mqttDown = true;
static bool mqttStable = false;
static uint32_t mqttFailStreak = 0;
static unsigned long mqttDownSince = 0;
static unsigned long mqttLastAttempt = 0;
static unsigned long mqttRetryDelay = 0;      // 0 = first attempt runs immediately
static unsigned long mqttConnectedAt = 0;

static void bumpMqttBackoff() {
  mqttRetryDelay = (mqttRetryDelay == 0) ? MQTT_RETRY_MIN_MS
                                         : min(mqttRetryDelay * 2, MQTT_RETRY_MAX_MS);
}

// One connection attempt. It still blocks for the TLS handshake (bounded by the timeouts
// above), but MQTT is down at that moment, so no WOL message can be missed.
static void connectMQTT(unsigned long now) {
  mqttLastAttempt = now;
  Serial.printf("[MQTT] Connecting as %s (fail streak %lu)...\n",
                clientId, (unsigned long)mqttFailStreak);
  unsigned long t0 = millis();
  bool ok = mqtt.connect(clientId, MQTT_USER, MQTT_PASS);
  unsigned long took = millis() - t0;

  if (ok) {
    mqtt.subscribe(topicCmd);
    mqtt.subscribe(topicThrottle);
    mqtt.subscribe(topicErrors);
    mqttDown = false;
    mqttStable = false;
    mqttFailStreak = 0;
    mqttConnectedAt = millis();
    mqttConnectCount++;
    Serial.printf("MQTT Connected Successfully! (%lums)\n", took);
  } else {
    mqttFailStreak++;
    bumpMqttBackoff();
    int st = mqtt.state();
    Serial.printf("MQTT Connection Failed, state: %d (%s) after %lums, retry in %lus\n",
                  st, mqttStateStr(st), took, mqttRetryDelay / 1000);
  }
}

// Returns true when MQTT is connected. Calls mqtt.loop() every pass.
static bool handleMqtt(unsigned long now) {
  if (mqtt.connected()) {
    if (!mqttStable && now - mqttConnectedAt >= MQTT_STABLE_MS) {
      mqttStable = true;
      mqttRetryDelay = 0;           // healthy for 60 s: forget earlier backoff
    }
    mqtt.loop();
    return true;
  }

  if (!mqttDown) {                  // connected -> lost transition
    mqttDown = true;
    mqttDownSince = now;
    mqttLastAttempt = now;          // wait one backoff step before reconnecting
    if (!mqttStable) bumpMqttBackoff();   // dropped quickly: treat as a failure
    else mqttRetryDelay = MQTT_RETRY_MIN_MS;
    Serial.printf("[MQTT] Connection lost, state %d (%s)\n",
                  mqtt.state(), mqttStateStr(mqtt.state()));
  }

  if (now - mqttDownSince >= MQTT_RESTART_MS) {
    restartWithCause(CAUSE_MQTT_TIMEOUT, "MQTT down > 10 min");
  }

  if (now - mqttLastAttempt >= mqttRetryDelay) {
    connectMQTT(now);
    return mqtt.connected();
  }
  return false;
}

// ================= Main =================
void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  ledWrite(true);

  // CHANGE: reset reason first, so a brownout is the first thing in the log.
  esp_reset_reason_t rr = esp_reset_reason();
  const char* rrs = resetReasonStr(rr);
  if (rr == ESP_RST_SW && rtcCauseMagic == RTC_CAUSE_MAGIC) {
    const char* why = (rtcCause == CAUSE_WIFI_TIMEOUT) ? "WIFI_TIMEOUT"
                    : (rtcCause == CAUSE_MQTT_TIMEOUT) ? "MQTT_TIMEOUT" : "OTHER";
    snprintf(resetReasonText, sizeof(resetReasonText), "%s:%s", rrs, why);
  } else {
    snprintf(resetReasonText, sizeof(resetReasonText), "%s", rrs);
  }
  rtcCauseMagic = 0;   // consume, so a later reset is not mislabelled
  delay(200);          // give USB-CDC serial a moment so this line is not lost
  Serial.printf("\n[BOOT] Reset reason: %s (%d)\n", resetReasonText, (int)rr);
  if (rr == ESP_RST_BROWNOUT) {
    Serial.println("[BOOT] BROWNOUT: supply sagged during a current spike. Check cable/PSU.");
  }

  WiFi.persistent(false);      // CHANGE: don't rewrite WiFi creds to NVS on every boot/restart
  WiFi.onEvent(onWiFiEvent);   // CHANGE: log connect/disconnect reasons
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

#if CONFIGURE_RF_SWITCH_PINS
  // REVIEW (CHANGE: flagged, not removed).
  // * Classic ESP32 / "ESP32 Dev Module": GPIO3 is U0RXD (UART0 RX). pinMode(3, OUTPUT) after
  //   Serial.begin() breaks serial *receive* (logging still works) and can fight the USB-UART
  //   chip's TX line. GPIO14 is a JTAG pin (MTMS) and is safe to drive.
  // * This exact sequence (GPIO3 LOW, GPIO14 HIGH) matches the Seeed XIAO ESP32-C6 RF switch:
  //   GPIO3 LOW enables the RF switch, GPIO14 HIGH selects the EXTERNAL U.FL antenna.
  //   With no external antenna attached, RSSI is very poor, a cause of disconnects in its own right.
  // Keep it if this is your board and you use an external antenna. Write 14 LOW for the built-in
  // antenna. Set CONFIGURE_RF_SWITCH_PINS to 0 on a classic ESP32.
  pinMode(3, OUTPUT);
  digitalWrite(3, LOW);
  delay(100);
  pinMode(14, OUTPUT);
  digitalWrite(14, HIGH);
  delay(100);
#endif

  WiFi.macAddress(mac);
  char macHex[13];
  snprintf(macHex, sizeof(macHex), "%02x%02x%02x%02x%02x%02x",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

  if (USE_HASHED_ID) sha256Hex(macHex, esp32mac);
  else strncpy(esp32mac, macHex, sizeof(esp32mac));

  snprintf(macColon, sizeof(macColon), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

  // CHANGE: stable client ID. If the broker still holds a half-dead session after a reboot,
  // reusing the ID makes it drop that session instead of keeping a ghost connection.
  snprintf(clientId, sizeof(clientId), "esp32wol-%.12s", esp32mac);

  snprintf(topicCmd,      sizeof(topicCmd),      "%s/feeds/wake-on-lan", MQTT_USER);
  snprintf(topicStatus,   sizeof(topicStatus),   "%s/feeds/pc-status",   MQTT_USER);
  snprintf(topicThrottle, sizeof(topicThrottle), "%s/throttle",          MQTT_USER);
  snprintf(topicErrors,   sizeof(topicErrors),   "%s/errors",            MQTT_USER);
  snprintf(topicDiag,     sizeof(topicDiag),     "%s/feeds/%s", MQTT_USER, DIAG_FEED_KEY);

  pcIpIsLiteral = pcIp.fromString(PC_IP_ADDRESS);

  WiFi.setAutoReconnect(true);
  WiFi.begin(SECRET_WIFI_NAME, SECRET_WIFI_PASSWORD);
  Serial.println("Attempting WiFi Connection");

  // Kept: a short blocking wait at boot (nothing to miss yet). loop() takes over after 10 s.
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    ledWrite(true);
    delay(250);
    ledWrite(false);
    delay(250);
    Serial.print(".");
    attempts++;
  }

  // NOTE: setInsecure() skips server-certificate checks (unchanged behaviour). See change list.
  espClient.setInsecure();
  espClient.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);   // CHANGE
  mqtt.setServer(MQTT_HOST, MQTT_PORT);                     // CHANGE: set once, not per attempt
  mqtt.setCallback(onMqttMessage);
  mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);             // CHANGE: default is 15 s
  mqtt.setBufferSize(512);   // CHANGE: AIO error/throttle texts can exceed the 256 B default

  mqttDownSince = millis();

  Serial.printf("\nSetup Complete (client %s)\n", clientId);
}

void loop() {
  // CHANGE: 1 ms yield. On single-core chips (C3/C6) a tight loop can starve the idle task.
  // Small enough that mqtt.loop() still runs ~1000x/s.
  delay(1);
  unsigned long now = millis();
  ledUpdate(now);   // CHANGE: runs every pass, so blinking never blocks mqtt.loop()

  if (!handleWiFi(now)) {
    ledSetBackground(&LED_WIFI_DOWN);
    return;
  }

  if (!handleMqtt(now)) {
    ledSetBackground(mqttFailStreak > 0 ? &LED_MQTT_FAIL : nullptr);
    return;
  }
  ledSetBackground(nullptr);   // all good: LED off

  if (isStartup) {
    Serial.println("Startup MQTT ping");
    mqtt.publish(topicStatus, "OFF");
    isStartup = false;
  }

#if PUBLISH_RESET_REASON
  if (!resetReasonPublished) {
    resetReasonPublished = mqtt.publish(topicDiag, resetReasonText);
    Serial.printf("[DIAG] Reset reason '%s' publish %s\n",
                  resetReasonText, resetReasonPublished ? "sent" : "FAILED");
    resetReasonPublished = true;   // once per boot even on failure: protects the rate budget
  }
#endif

  if (now - lastPingTime >= PING_INTERVAL) {
    lastPingTime = now;

    // CHANGE: 1 s explicit timeout (ms on both 2.x and 3.x) so a missing PC can't stall loop().
    WiFiClient pingClient;
    unsigned long t0 = millis();
    bool pingSuccess = pcIpIsLiteral
        ? pingClient.connect(pcIp, MOONLIGHT_PORT, PING_TIMEOUT_MS)
        : pingClient.connect(PC_IP_ADDRESS, MOONLIGHT_PORT, PING_TIMEOUT_MS);
    pingClient.stop();
    logHealth(pingSuccess, millis() - t0);

    if (pingSuccess && !isPcOn) {
      isPcOn = true;
      mqtt.publish(topicStatus, "ON");
      Serial.println("PC is now ON");
    }
    else if (!pingSuccess && isPcOn) {
      isPcOn = false;
      mqtt.publish(topicStatus, "OFF");
      Serial.println("PC Heartbeat timeout");
      Serial.println("PC is now OFF");
      ledPlayOnce(&LED_PC_OFF);    // same 3 slow blinks, non-blocking
    }
    else if (pingSuccess && isPcOn) {
      mqtt.publish(topicStatus, "ON");
      Serial.println("PC heartbeat found");
    }
  }
}

/* ======================================================================
   CHANGE LIST
   ----------------------------------------------------------------------
   Diagnostics
    1. Boot log prints esp_reset_reason() as text (BROWNOUT, PANIC, TASK_WDT...). Self-initiated
       restarts are tagged SOFTWARE:WIFI_TIMEOUT / SOFTWARE:MQTT_TIMEOUT through RTC memory.
    2. WiFi event handler logs association, IP+RSSI, and disconnect reason code + name.
    3. Each heartbeat logs PC state, probe time, RSSI, free/min heap, uptime, disconnect count,
       last disconnect reason, and MQTT connect count.
    4. Reset reason published once per boot to <user>/feeds/esp32-wol-diag (needs 1 free feed slot;
       set PUBLISH_RESET_REASON 0 to disable).
    5. Subscribes to <user>/throttle and <user>/errors and prints them.
   WiFi
    6. Non-blocking WiFi.reconnect() with 2-16 s backoff; ESP.restart() after 60 s down.
    7. WiFi.persistent(false), setAutoReconnect(true) made explicit.
   MQTT
    8. Blocking delay(2000)+blinkLed() removed; millis() retry with 2-60 s exponential backoff,
       reset only after 60 s of stable connection (flap protection). About 5 attempts/min max.
    9. Stable client ID "esp32wol-<12 hex of hashed MAC>" replaces random().
   10. Socket timeout 10 s, TLS handshake timeout 10 s, buffer 512 B, server/callback set once.
   11. ESP.restart() if MQTT can't connect for 10 min.
   12. WOL feed matched by suffix "wake-on-lan" (as in the original). MAC parsed tolerantly from
       the full payload (':' / '-' / bare hex; ignores quotes, whitespace, JSON). Every message is
       logged with topic and hex dump. WOL packet is sent before the LED feedback.
   LED
   13. millis() state machine with the same patterns: off=OK, 1 Hz=WiFi down, double=MQTT fail,
       3 slow=PC off (one-shot), 5 fast=WOL (one-shot). mqtt.loop() is never blocked by blinking.
       LED_ACTIVE_LOW option for boards with inverted LEDs.
   Heartbeat
   14. TCP probe uses a 1000 ms timeout and a pre-parsed IPAddress (DNS fallback if a hostname).
   Pins
   15. GPIO3/GPIO14 writes kept behind CONFIGURE_RF_SWITCH_PINS with a REVIEW comment.
   Unchanged on purpose
    - Topics, payloads ("ON"/"OFF"), 10 s heartbeat, WOL packet format, startup "OFF" publish,
      secrets.h usage, setInsecure() (consider setCACert() with Adafruit IO's root CA).
   ====================================================================== */
