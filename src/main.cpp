/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  RainwaterIOT — ESP32 Bridge Firmware
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  ROLE:
 *    This ESP32 acts as a WiFi bridge between the Arduino Mega (which handles
 *    all sensor reading and actuator control) and the outside world (MongoDB
 *    backend, mobile app, MQTT broker).
 *
 *  DATA FLOW:
 *
 *    Arduino Mega                  ESP32                         Broker / Backend
 *    ────────────                  ─────                         ────────────────
 *    Sensor data  →  Serial2  →  MQTT publish  →  rainwater/sensors          →  Backend → MongoDB
 *    Actuators    ←  Serial2  ←  MQTT subscribe ←  rainwater/commands         ←  Backend ← App
 *    ACKs         →  Serial2  →  MQTT publish  →  rainwater/acks             →  Backend
 *    Cal cmds     ←  Serial2  ←  MQTT subscribe ←  rainwater/calibration/commands ←  Backend ← App
 *    Cal ACKs     →  Serial2  →  MQTT publish  →  rainwater/calibration/acks →  Backend
 *
 *  PROTOCOL (Mega → ESP32 over Serial2):
 *    Each line is:  "S,<KEY>,<VALUE>\n"
 *    Examples:
 *      S,FLOW,1.23
 *      S,LVL_C2,45.6
 *      S,TEMP_C5,28.3
 *      S,PH_C5,7.21
 *      S,TURB_C5,2.4
 *      S,STATE,0,1,0     ← first_flush_state, filter_mode, backwash_state
 *
 *  PROTOCOL (ESP32 → Mega over Serial2):
 *    Each line is:  "C,<COMMAND>,<PARAM>\n"
 *    Examples:
 *      C,FILTER,CHARCOAL
 *      C,FILTER,BOTH
 *      C,BACKWASH,START
 *      C,BACKWASH,STOP
 *      C,ESTOP,ON
 *      C,ESTOP,OFF
 *      C,CAL_PH,C2,MID
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ESPAsyncWebServer.h>
#include <WebSerialLite.h>
#include "config.h"

// ── WebSerial async server ────────────────────────────────────────────────────
AsyncWebServer wsServer(80);

// mqttClient is defined below; forward-declared here so wsLogf can reference it.
extern PubSubClient mqttClient;

// Guard: set true while inside the MQTT callback so wsLogf skips the
// re-entrant publish (PubSubClient::publish() overwrites the same internal
// _buffer that the callback's `topic` pointer still references).
static bool mqttCallbackActive = false;

// Print to Serial, WebSerial, and publish to rainwater/logs.
// wsLogf is the canonical sink; wsLog/wsLogln are thin wrappers around it.
void wsLogf(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print(buf);
    WebSerial.print(buf);
    if (mqttCallbackActive) return;   // never publish from within the callback
    // Strip trailing newline — MQTT carries one message per publish
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    if (mqttClient.connected()) mqttClient.publish("rainwater/logs", buf);
}
template<typename T>
void wsLog(T msg) { wsLogf("%s", String(msg).c_str()); }
template<typename T>
void wsLogln(T msg) { wsLogf("%s\n", String(msg).c_str()); }

// ═════════════════════════════════════════════════════════════════════════════
//  HTTP helpers — fire-and-forget POSTs to the Vercel backend
//  WiFiClient is allocated on the stack; each call opens + closes its own
//  connection (acceptable for low-frequency event/log posts).
// ═════════════════════════════════════════════════════════════════════════════
void postHttp(const char* path, const char* body)
{
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure httpTls;
    httpTls.setInsecure();   // same trust policy as MQTT
    HTTPClient http;

    String url = String(API_BASE) + path;
    if (!http.begin(httpTls, url)) {
        wsLogf("[HTTP] begin() failed for %s\n", path);
        return;
    }
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(body);
    if (code > 0) {
        wsLogf("[HTTP] POST %s → %d\n", path, code);
    } else {
        wsLogf("[HTTP] POST %s error: %s\n", path, http.errorToString(code).c_str());
    }
    http.end();
}

// Post a structured activity log entry
// raw format: "L,<LEVEL>,<CATEGORY>,<message>"  (mirrors Mega→ESP32 log protocol)
void postActivity(const char* level, const char* category,
                  const char* message, const char* source)
{
    char raw[220];
    snprintf(raw, sizeof(raw), "L,%s,%s,[%s] %s", level, category, source, message);

    char body[256];
    snprintf(body, sizeof(body), "{\"raw\":\"%s\"}", raw);
    postHttp("/api/activity", body);
}

// Post a raw ACK string
void postAck(const String& raw)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"raw\":\"%s\"}", raw.c_str());
    postHttp("/api/acks", body);
}

// ── MQTT ──────────────────────────────────────────────────────────────────────
WiFiClientSecure secureClient;
PubSubClient     mqttClient(secureClient);
unsigned long    lastMqttReconnectMs = 0;

// ── Serial2 link to Arduino Mega ─────────────────────────────────────────────
HardwareSerial MegaSerial(2);   // UART2 — GPIO16 (RX), GPIO17 (TX)

// ── Sensor data store ─────────────────────────────────────────────────────────
StaticJsonDocument<512> sensorDoc;
bool sensorDataReady = false;

// ── Pending MQTT commands from callback → loop() ──────────────────────────────
// PubSubClient must not publish/subscribe from within its own callback.
// Commands are queued here and drained in loop() after mqttClient.loop() returns.
#define CMD_QUEUE_SIZE 8
struct QueuedCmd {
    String  cmd;
    bool    isCalibration;   // true → ACK goes to rainwater/calibration/acks
};
QueuedCmd cmdQueue[CMD_QUEUE_SIZE];
uint8_t   cmdQueueLen = 0;

// ── Timing ────────────────────────────────────────────────────────────────────
unsigned long lastPostMs = 0;

// ═════════════════════════════════════════════════════════════════════════════
//  WiFi
// ═════════════════════════════════════════════════════════════════════════════
void connectWiFi()
{
    wsLog(F("[WiFi] Connecting to "));
    wsLogln(WIFI_SSID);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint8_t attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(WIFI_RETRY_DELAY_MS);
        wsLog('.');
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        wsLogln("");
        wsLog(F("[WiFi] Connected — IP: "));
        wsLogln(WiFi.localIP());
        postActivity("INFO", "NETWORK", "WiFi connected", "esp32");
    } else {
        wsLogln("");
        wsLogln(F("[WiFi] FAILED — will retry in loop"));
        postActivity("WARN", "NETWORK", "WiFi connect failed", "esp32");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Parse a single frame line from the Mega
//
//  Handled prefixes:
//    S,KEY,VALUE          — sensor reading   → sensorDoc
//    L,LEVEL,CATEGORY,MSG — log entry        → POST /api/activity (source: mega)
//    A,<rest>             — ACK from Mega    → POST /api/acks + MQTT rainwater/acks
//                           (CAL ACKs: A,CAL_* → rainwater/calibration/acks)
//    C,<rest>             — command echo     → logged only (Mega echoes back commands)
// ═════════════════════════════════════════════════════════════════════════════
void parseMegaLine(const String& line)
{
    // ── S, sensor frame ──────────────────────────────────────────────────────
    if (line.startsWith("S,")) {
        int firstComma  = line.indexOf(',');
        int secondComma = line.indexOf(',', firstComma + 1);
        if (secondComma < 0) return;

        String key   = line.substring(firstComma + 1, secondComma);
        String value = line.substring(secondComma + 1);
        value.trim();

        if (key == "STATE") {
            sensorDoc["ff_state"]       = value.substring(0, value.indexOf(',')).toInt();
            int s2 = value.indexOf(',') + 1;
            int s3 = value.indexOf(',', s2);
            sensorDoc["filter_mode"]    = value.substring(s2, s3).toInt();
            sensorDoc["backwash_state"] = value.substring(s3 + 1).toInt();
            sensorDataReady = true;
        } else {
            sensorDoc[key] = value.toFloat();
        }

        wsLog(F("[Mega] "));
        wsLog(key);
        wsLog(F(" = "));
        wsLogln(value);
        return;
    }

    // ── L, log frame  ────────────────────────────────────────────────────────
    // Format: L,LEVEL,CATEGORY,message text (message may contain commas)
    if (line.startsWith("L,")) {
        int c1 = line.indexOf(',');           // after 'L'
        int c2 = line.indexOf(',', c1 + 1);   // after LEVEL
        int c3 = line.indexOf(',', c2 + 1);   // after CATEGORY
        if (c3 < 0) return;

        String level    = line.substring(c1 + 1, c2);
        String category = line.substring(c2 + 1, c3);
        String message  = line.substring(c3 + 1);
        message.trim();

        wsLogf("[Mega/%s] %s: %s\n", category.c_str(), level.c_str(), message.c_str());
        postActivity(level.c_str(), category.c_str(), message.c_str(), "mega");
        return;
    }

    // ── A, ACK frame  ────────────────────────────────────────────────────────
    // Format: A,VALVE,V1,OK  or  A,CAL_PH,C2,MID,OK,2.53
    // ACKs that arrive here are unsolicited (outside a drainCommandQueue window).
    // They are still POSTed to the HTTP endpoint and published to MQTT.
    if (line.startsWith("A,")) {
        wsLog(F("[Mega] ACK: "));
        wsLogln(line);

        postAck(line);

        if (mqttClient.connected()) {
            const char* ackTopic = line.startsWith("A,CAL_")
                                   ? MQTT_TOPIC_CAL_ACKS
                                   : MQTT_TOPIC_ACKS;
            mqttClient.publish(ackTopic, line.c_str(), false);
        }
        return;
    }

    // ── C, command echo  ─────────────────────────────────────────────────────
    if (line.startsWith("C,")) {
        wsLog(F("[Mega] Echo: "));
        wsLogln(line);
        return;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Enqueue commands from a raw payload string
//  Handles both JSON array {"commands":[...]} and plain "C,..." strings
// ═════════════════════════════════════════════════════════════════════════════
void enqueuePayload(const char* buf, bool isCalibration)
{
    JsonDocument doc;
    DeserializationError jsonErr = deserializeJson(doc, buf);

    if (jsonErr != DeserializationError::Ok) {
        // Plain string command
        String raw = String(buf);
        raw.trim();
        if (raw.length() > 0 && cmdQueueLen < CMD_QUEUE_SIZE) {
            cmdQueue[cmdQueueLen++] = { raw, isCalibration };
        }
        return;
    }

    JsonArray cmds = doc["commands"].as<JsonArray>();
    if (cmds.size() == 0) { wsLogln(F("[MQTT] commands array empty")); return; }

    for (JsonVariant entry : cmds) {
        if (cmdQueueLen >= CMD_QUEUE_SIZE) break;
        String cmd = entry.as<String>();
        if (cmd.length() > 0) cmdQueue[cmdQueueLen++] = { cmd, isCalibration };
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  MQTT — incoming message callback
//  Only enqueues — no mqttClient publish/subscribe calls allowed here.
//  NOTE: `topic` and `payload` point into PubSubClient's internal _buffer.
//  Copy both to locals immediately before doing anything else.
// ═════════════════════════════════════════════════════════════════════════════
void onMqttMessage(char* topic, byte* payload, unsigned int length)
{
    // Copy topic — PubSubClient::publish() would overwrite _buffer (and topic)
    char topicBuf[64];
    strncpy(topicBuf, topic, sizeof(topicBuf) - 1);
    topicBuf[sizeof(topicBuf) - 1] = '\0';

    char buf[length + 1];
    memcpy(buf, payload, length);
    buf[length] = '\0';

    mqttCallbackActive = true;
    wsLogf("[MQTT] %s → %s\n", topicBuf, buf);
    mqttCallbackActive = false;

    if (strcmp(topicBuf, MQTT_TOPIC_COMMANDS) == 0) {
        enqueuePayload(buf, false);
    } else if (strcmp(topicBuf, MQTT_TOPIC_CAL_COMMANDS) == 0) {
        enqueuePayload(buf, true);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Drain command queue — called from loop() after mqttClient.loop() returns
//  Blasts all queued commands to Mega, then collects ACKs dynamically
// ═════════════════════════════════════════════════════════════════════════════
void drainCommandQueue()
{
    if (cmdQueueLen == 0) return;

    uint8_t count = cmdQueueLen;
    cmdQueueLen = 0;  // clear before drain so re-entrant calls don't re-send

    // Blast all commands to Mega immediately
    for (uint8_t i = 0; i < count; i++) {
        wsLogf("[CMD] -> Mega: %s\n", cmdQueue[i].cmd.c_str());
        MegaSerial.println(cmdQueue[i].cmd);
    }

    // Dynamic ACK drain — collect ACKs in order, exit when all received or 50ms elapsed
    unsigned long drainUntil   = millis() + 50;
    uint8_t       acksReceived = 0;

    while (millis() < drainUntil && acksReceived < count) {
        if (!MegaSerial.available()) continue;

        String line = MegaSerial.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        if (line.startsWith("A,")) {
            wsLogf("[CMD] ACK OK  (%s) → %s\n",
                   cmdQueue[acksReceived].cmd.c_str(), line.c_str());
            const char* ackTopic = cmdQueue[acksReceived].isCalibration
                                   ? MQTT_TOPIC_CAL_ACKS
                                   : MQTT_TOPIC_ACKS;
            mqttClient.publish(ackTopic, line.c_str());
            postAck(line);
            acksReceived++;
        } else {
            parseMegaLine(line);
        }
    }

    // Report each command that never received an ACK
    for (uint8_t i = acksReceived; i < count; i++) {
        wsLogf("[CMD] TIMEOUT — no ACK for: %s\n", cmdQueue[i].cmd.c_str());
        postActivity("WARN", "COMMAND",
                     (String("No ACK: ") + cmdQueue[i].cmd).c_str(), "esp32");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  MQTT — connect / reconnect (non-blocking, rate-limited)
// ═════════════════════════════════════════════════════════════════════════════
void reconnectMQTT()
{
    if (mqttClient.connected()) return;
    if (millis() - lastMqttReconnectMs < MQTT_RECONNECT_MS) return;
    lastMqttReconnectMs = millis();

    wsLogf("[MQTT] Connecting to %s...\n", MQTT_BROKER);
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
        wsLogln(F("[MQTT] Connected"));
        mqttClient.subscribe(MQTT_TOPIC_COMMANDS);
        mqttClient.subscribe(MQTT_TOPIC_CAL_COMMANDS);
        wsLogln(F("[MQTT] Subscribed to commands + calibration/commands"));
        postActivity("INFO", "NETWORK", "MQTT connected", "esp32");
    } else {
        wsLogf("[MQTT] Failed, rc=%d — will retry in %ds\n",
               mqttClient.state(), MQTT_RECONNECT_MS / 1000);
        postActivity("WARN", "NETWORK", "MQTT connect failed", "esp32");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Publish sensor data to MQTT broker
// ═════════════════════════════════════════════════════════════════════════════
void publishSensorData()
{
    if (!sensorDataReady) return;
    if (!mqttClient.connected()) return;

    sensorDoc["uptime_ms"] = millis();

    char body[512];
    serializeJson(sensorDoc, body, sizeof(body));

    if (mqttClient.publish(MQTT_TOPIC_SENSORS, body)) {
        wsLogln(F("[MQTT] Sensors published"));
    } else {
        wsLogln(F("[MQTT] Sensor publish failed"));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println(F("=========================================================="));
    Serial.println(F("  RainwaterIOT — ESP32 Bridge"));
    Serial.println(F("  Booting..."));
    Serial.println(F("=========================================================="));

    // Serial2 link to Arduino Mega
    MegaSerial.begin(MEGA_BAUD_RATE, SERIAL_8N1, MEGA_RX_PIN, MEGA_TX_PIN);
    Serial.println(F("[Init] Serial2 (Mega link) started"));

    // WiFi
    connectWiFi();

    // TLS — skip cert verification (acceptable for thesis prototype)
    secureClient.setInsecure();

    // MQTT
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(onMqttMessage);
    mqttClient.setBufferSize(512);
    reconnectMQTT();

    // OTA
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setMdnsEnabled(false);
    ArduinoOTA.onStart([]() {
        wsLogln(F("[OTA] Start"));
        postActivity("INFO", "SYSTEM", "OTA update started", "esp32");
    });
    ArduinoOTA.onEnd([]() {
        wsLogln(F("[OTA] Done — rebooting"));
        postActivity("INFO", "SYSTEM", "OTA update completed", "esp32");
    });
    ArduinoOTA.onError([](ota_error_t error) {
        wsLog(F("[OTA] Error: "));
        wsLogln(error);
        postActivity("ERROR", "SYSTEM", "OTA update failed", "esp32");
    });
    ArduinoOTA.begin();
    wsLogln(F("[Init] OTA ready — hostname: " OTA_HOSTNAME));

    // WebSerial
    WebSerial.begin(&wsServer);
    wsServer.begin();
    Serial.print(F("[Init] WebSerial at http://"));
    Serial.print(WiFi.localIP());
    Serial.println(F("/webserial"));

    wsLogln(F("=========================================================="));
    wsLogln(F("  ESP32 Ready"));
    wsLogln(F("=========================================================="));
    postActivity("INFO", "SYSTEM", "ESP32 boot complete", "esp32");
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════════════════════
void loop()
{
    unsigned long now = millis();

    // ── 0. OTA ───────────────────────────────────────────────────────────────
    ArduinoOTA.handle();

    // ── 1. WiFi reconnect ────────────────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
        wsLogln(F("[WiFi] Disconnected — reconnecting..."));
        postActivity("WARN", "NETWORK", "WiFi disconnected", "esp32");
        connectWiFi();
    }

    // ── 2. MQTT loop + reconnect ─────────────────────────────────────────────
    if (!mqttClient.loop()) {
        postActivity("WARN", "NETWORK", "MQTT disconnected", "esp32");
        reconnectMQTT();
    }
    drainCommandQueue();

    // ── 3. Read sensor lines from Mega ───────────────────────────────────────
    while (MegaSerial.available()) {
        String line = MegaSerial.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) parseMegaLine(line);
    }

    // ── 4. Publish sensor data ───────────────────────────────────────────────
    if ((now - lastPostMs) >= POST_INTERVAL_MS) {
        lastPostMs = now;
        publishSensorData();
    }
}
