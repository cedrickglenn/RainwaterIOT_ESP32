#ifndef CONFIG_H
#define CONFIG_H

// ── WiFi credentials ────────────────────────────────────────────────────────
#define WIFI_SSID       "GlobeAtHome_e9528"
#define WIFI_PASSWORD   "K4QQ6DXr"

// ── MQTT ─────────────────────────────────────────────────────────────────────
#define MQTT_BROKER     "751cf20bd96c44afa0f25b4da8da918d.s1.eu.hivemq.cloud"   // ← replace
#define MQTT_PORT       8883
#define MQTT_USER       "rainwaterIOT"                      // ← replace
#define MQTT_PASSWORD   "6O3`$G£)1Vg6"                      // ← replace
#define MQTT_CLIENT_ID      "rainwater_esp32"
#define MQTT_TOPIC_SENSORS      "rainwater/sensors"
#define MQTT_TOPIC_COMMANDS     "rainwater/commands"
#define MQTT_TOPIC_ACKS         "rainwater/acks"
#define MQTT_TOPIC_CAL_COMMANDS "rainwater/calibration/commands"
#define MQTT_TOPIC_CAL_ACKS     "rainwater/calibration/acks"

// ── Serial link to Arduino Mega ─────────────────────────────────────────────
#define MEGA_RX_PIN     16    // ESP32 GPIO16 ← Mega TX1 (pin 18) via voltage divider
#define MEGA_TX_PIN     17    // ESP32 GPIO17 → Mega RX1 (pin 19) direct
#define MEGA_BAUD_RATE  115200

// ── OTA ──────────────────────────────────────────────────────────────────────
#define OTA_HOSTNAME    "rainwater-esp32"

// ── Timing ───────────────────────────────────────────────────────────────────
#define POST_INTERVAL_MS        1000   // Publish sensor data every 1s
#define HEARTBEAT_INTERVAL_MS  30000   // POST /api/heartbeat every 30s
#define WIFI_RETRY_DELAY_MS     500    // Delay between WiFi connection retries
#define MQTT_RECONNECT_MS      5000    // Min interval between MQTT reconnect attempts
#define MQTT_KEEPALIVE_S         20    // MQTT keep-alive interval in seconds (HiveMQ will ping ESP32 at 1.5× this)
#define MQTT_PUBLISH_FAIL_MAX     3    // Force reconnect after this many consecutive publish failures
#define MQTT_SOCKET_COOLDOWN_MS 8000   // Wait after secureClient.stop() before reconnecting — lets Railway's proxy clear TIME_WAIT
#define MQTT_RECONNECT_FAIL_MAX   5    // Force WiFi reconnect after this many consecutive MQTT connect() failures

#endif // CONFIG_H
