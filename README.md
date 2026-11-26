# ESP32 Remote Wake-on-LAN

Wake your PC from anywhere. An ESP32 on your home network listens for a command on an
[Adafruit IO](https://io.adafruit.com) feed and broadcasts a Wake-on-LAN magic packet.
It also reports whether the PC is on, so a dashboard can show its state.

No port forwarding, no VPN, no cloud server of your own. The ESP32 makes an outbound
TLS connection to Adafruit IO and waits for messages.

Based on [kreaxv/esp32-remote-wol](https://github.com/kreaxv/esp32-remote-wol), extended
with automatic recovery, diagnostics and non-blocking operation for long unattended uptime.

---

## Features

- **Remote wake.** Publish your PC's MAC address to the `wake-on-lan` feed and the ESP32 sends
  a standard magic packet (UDP broadcast, port 9) on your LAN.
- **PC status.** Every 10 s the ESP32 tries a TCP connection to the PC's Moonlight /
  Sunshine port (47989). It publishes `ON` or `OFF` to the `pc-status` feed.
- **Self-healing.**
  - WiFi reconnects with backoff. The board restarts if WiFi stays down for more than 60 s.
  - MQTT reconnects with exponential backoff (2 s to 60 s), staying well inside Adafruit IO's
    free-tier limits. The board restarts if MQTT stays down for more than 10 minutes.
  - Every network operation has a timeout, so nothing can stall the main loop.
- **Diagnostics.**
  - Prints the reset reason at boot, so brownouts and crashes are easy to spot.
  - Logs WiFi disconnect reason codes.
  - Each heartbeat logs RSSI, free heap, uptime and reconnect counters.
  - Forwards Adafruit IO `throttle` and `errors` messages to the serial log.
  - Optionally publishes the last reset reason to a feed.
- **Non-blocking LED status.** The LED blinks without pausing the program, so a wake command
  is never missed.
- **Tolerant MAC input.** Accepts `AA:BB:CC:DD:EE:FF`, `AA-BB-CC-DD-EE-FF` or `AABBCCDDEEFF`.
  Quotes, whitespace and JSON wrappers are ignored.

## How it works

```
 Phone / dashboard ──► Adafruit IO ◄──TLS/MQTT── ESP32 ──UDP broadcast──► PC (wakes)
                       feeds:                       │
                        wake-on-lan  (command)      └──TCP :47989 probe──► PC (on/off?)
                        pc-status    (ON/OFF)
                        esp32-wol-diag (optional)
```

## Requirements

**Hardware**
- An ESP32 board on the same LAN/subnet as the PC. Magic packets are broadcasts and do not
  cross routers.
- A reliable USB power supply and a short, good-quality cable. The WiFi radio draws current
  spikes, and weak power causes brownout resets.

**PC**
- Wake-on-LAN enabled in BIOS/UEFI and in the network adapter's settings. A wired Ethernet
  connection is strongly recommended; WiFi wake is rarely supported.
- For status reporting: [Sunshine](https://github.com/LizardByte/Sunshine) or another
  GameStream host listening on TCP 47989. You can change `MOONLIGHT_PORT` to any port your PC
  always has open.

**Software**
- Arduino IDE 2.x or PlatformIO
- [arduino-esp32](https://github.com/espressif/arduino-esp32) core 2.x or 3.x
- [PubSubClient](https://github.com/knolleary/pubsubclient) 2.8 or newer
- A free Adafruit IO account

## Setup

1. **Adafruit IO.** Create the feeds `wake-on-lan` and `pc-status`. Optionally create
   `esp32-wol-diag`; it is also created automatically if you have a free feed slot.
   Free accounts allow 10 feeds.
2. **Secrets.** Copy `secrets.h.example` to `secrets.h` and fill it in:

   ```cpp
   #define SECRET_WIFI_NAME     "your-ssid"
   #define SECRET_WIFI_PASSWORD "your-wifi-password"
   #define SECRET_MQTT_USER     "your-adafruit-io-username"
   #define SECRET_MQTT_PASS     "your-adafruit-io-key"
   #define SECRET_PC_IP         "192.168.1.50"
   ```

   `secrets.h` is listed in `.gitignore`. **Never commit it.** Give the PC a static IP or a DHCP
   reservation so `SECRET_PC_IP` stays correct.
3. **Board options.** Set these at the top of the `.ino` file:

   | Option | Default | Purpose |
   |---|---|---|
   | `CONFIGURE_RF_SWITCH_PINS` | `1` | Drives GPIO3 LOW / GPIO14 HIGH (XIAO ESP32-C6 RF switch, external antenna). **Set to `0` on a classic ESP32**, where GPIO3 is UART0 RX. |
   | `LED_ACTIVE_LOW` | `0` | Set to `1` if your board's LED lights when the pin is LOW. |
   | `PUBLISH_RESET_REASON` | `1` | Publishes the reset reason once per boot. Set to `0` to save a feed. |
   | `USE_HASHED_ID` | `1` | Derives the MQTT client ID from a SHA-256 of the MAC instead of the raw MAC. |

4. **Flash.** Build for your board and upload. Open the serial monitor at 115200 baud.
5. **Test.** In Adafruit IO, add a value to `wake-on-lan` containing your PC's MAC address.
   The serial log should show `WOL Packet Sent to AA:BB:...`.

## LED status

| Pattern | Meaning |
|---|---|
| Off | Everything OK |
| Steady 1 Hz blink | WiFi disconnected |
| Double blink, repeating | MQTT connection failing |
| 3 slow blinks (once) | PC just went offline |
| 5 fast blinks (once) | Wake packet sent |

## Serial diagnostics

Typical lines and what they tell you:

```
[BOOT] Reset reason: BROWNOUT (9)            ← power supply or cable problem
[WiFi] Disconnected, reason 200 (BEACON_TIMEOUT)
[HB] PC=ON probe=8ms RSSI=-58dBm heap=201k (min 180k) up=0d03:12:44 wifiDisc=1 last=200(BEACON_TIMEOUT) mqttConn=2
[AIO THROTTLE] ...                           ← you exceeded Adafruit IO rate limits
MQTT Message Received on 'user/feeds/wake-on-lan' (17 bytes): AA:BB:CC:DD:EE:FF
```

**Troubleshooting**
- **`BROWNOUT` resets:** use a better cable or power supply.
- **RSSI below about −75 dBm:** move the board, or check the antenna. On boards with an antenna
  switch, make sure the selected antenna is actually connected.
- **`BAD_CREDENTIALS`:** check your Adafruit IO username and key.
- **`SOFTWARE:WIFI_TIMEOUT` or `SOFTWARE:MQTT_TIMEOUT`:** the watchdog logic restarted the board
  after a long outage. This is expected if the router or Adafruit IO was down.
- **Wake packet sent but the PC stays off:** check the BIOS and NIC Wake-on-LAN settings, Windows
  Fast Startup, and that both devices are on the same subnet.

## Adafruit IO usage

At most about 6 publishes per minute: one heartbeat every 10 s while the PC is on, plus one
diagnostic publish per boot. MQTT reconnects are capped at about 5 attempts in the first minute
of an outage, then about 1 per minute. Both stay well under the free tier's limits.

## Security notes

- Anyone who can publish to your `wake-on-lan` feed can wake your PC. Keep your Adafruit IO key
  private, and regenerate it if it is ever exposed.
- The TLS connection uses `setInsecure()`, which does **not** verify Adafruit IO's certificate.
  On an untrusted network, someone could impersonate the server and capture your key. For
  stronger security, replace it with `espClient.setCACert(...)` and Adafruit IO's root CA.
- The ESP32 only makes outbound connections. No ports are opened on your router.

## Credits and license

- Original project: [kreaxv/esp32-remote-wol](https://github.com/kreaxv/esp32-remote-wol)
- Robustness and diagnostics rework: Nicholas Persson, 2026
- License: _choose one and add a `LICENSE` file. Check that it is compatible with the original
  project's license._
