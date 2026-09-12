#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include "mbedtls/md.h"
#include "secrets.h"

// #define LED_BUILTIN 2
#define USE_HASHED_ID 1

// Nicholas Persson 2026
// Remote Wake on Lan for ESP32
// Based on the work of https://github.com/kreaxv/esp32-remote-wol
// May need some tweaks for individual use

// ----------------------- MQTT -----------------------
static const char* MQTT_HOST = "io.adafruit.com";
static const uint16_t MQTT_PORT = 8883;
static const char* MQTT_USER = SECRET_MQTT_USER;
static const char* MQTT_PASS = SECRET_MQTT_PASS;

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

// --------------------- LED Helper ---------------------
static void blinkLed(int times, int speedMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(speedMs);
    digitalWrite(LED_BUILTIN, LOW);
    delay(speedMs);
  }
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

// --------------------- WOL ---------------------
static void sendWOL(const char* macStr) {
  uint8_t targetMac[6];
  for (int i = 0; i < 6; i++) {
    targetMac[i] = (hexNibble(macStr[i*3]) << 4) | hexNibble(macStr[i*3 + 1]);
  }

  udp.beginPacket(WiFi.broadcastIP(), 9);
  for (int i = 0; i < 6; i++) udp.write(0xFF);
  for (int i = 0; i < 16; i++) udp.write(targetMac, 6);
  udp.endPacket();

  Serial.println("WOL Packet Sent");
}

// --------------------- MQTT ---------------------
static void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  char msg[32] = {0};
  int len = length < sizeof(msg)-1 ? length : sizeof(msg)-1;
  memcpy(msg, payload, len);
  Serial.println("MQTT Message Received");

  if (String(topic).endsWith("wake-on-lan")) {
    if (len >= 17 && String(msg).indexOf(':') != -1) {
      Serial.println("WOL Trigger Received");
      blinkLed(5, 50);
      sendWOL(msg);
      digitalWrite(LED_BUILTIN, LOW);
    } else {
      Serial.println("Invalid MAC payload rejected.");
    }
  }
}

static void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "ESP32Client-%04X", random(0xffff));
  snprintf(topicCmd, sizeof(topicCmd), "%s/feeds/wake-on-lan", MQTT_USER);

  if (mqtt.connect(clientId, MQTT_USER, MQTT_PASS)) {
    mqtt.subscribe(topicCmd);
    Serial.println("MQTT Connected Successfully!");
  } else {
    Serial.print("MQTT Connection Failed, state: ");
    Serial.println(mqtt.state());
    blinkLed(2, 500);
    delay(2000);
  }
}

// ================= Main =================
void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  pinMode(3, OUTPUT);
  digitalWrite(3, LOW);
  delay(100);
  pinMode(14, OUTPUT);
  digitalWrite(14, HIGH);
  delay(100);

  WiFi.macAddress(mac);
  char macHex[13];
  snprintf(macHex, sizeof(macHex), "%02x%02x%02x%02x%02x%02x",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

  if (USE_HASHED_ID) sha256Hex(macHex, esp32mac);
  else strncpy(esp32mac, macHex, sizeof(esp32mac));

  snprintf(macColon, sizeof(macColon), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

  WiFi.begin(SECRET_WIFI_NAME, SECRET_WIFI_PASSWORD);
  Serial.println("Attempting WiFi Connection");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(250);
    digitalWrite(LED_BUILTIN, LOW);
    delay(250);
    Serial.print(".");
    attempts++;
  }

  espClient.setInsecure();

  Serial.println("\nSetup Complete");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      connectMQTT();
    } else {
      digitalWrite(LED_BUILTIN, LOW);

      mqtt.loop();

      char statusTopic[80];
      snprintf(statusTopic, sizeof(statusTopic), "%s/feeds/pc-status", MQTT_USER);

      if (isStartup) {
        Serial.println("Startup MQTT ping");
        mqtt.publish(statusTopic, "OFF");
        isStartup = false;
      }

      unsigned long currentMillis = millis();
      if (currentMillis - lastPingTime >= PING_INTERVAL) {
        lastPingTime = currentMillis;

        WiFiClient pingClient;
        bool pingSuccess = pingClient.connect(PC_IP_ADDRESS, MOONLIGHT_PORT);
        pingClient.stop();

        if (pingSuccess && !isPcOn) {
          isPcOn = true;
          mqtt.publish(statusTopic, "ON");
          Serial.println("PC is now ON");
        }
        else if (!pingSuccess && isPcOn) {
          isPcOn = false;
          mqtt.publish(statusTopic, "OFF");
          Serial.println("PC Heartbeat timeout");
          Serial.println("PC is now OFF");

          blinkLed(3, 500);
          digitalWrite(LED_BUILTIN, LOW);
        }
        else if (pingSuccess && isPcOn) {
          mqtt.publish(statusTopic, "ON");
          Serial.println("PC heartbeat found");
        }
      }
    }
  }
  else {
    Serial.println("ERROR - WIFI DISCONNECTED");
    blinkLed(1, 500);
  }
}
