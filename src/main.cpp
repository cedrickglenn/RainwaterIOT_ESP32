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
 *    ACKs         →  Serial2  →  MQTT publish  →  rainwater/acks             →  mqtt-bridge.js → MongoDB
 *    Cal cmds     ←  Serial2  ←  MQTT subscribe ←  rainwater/calibration/commands ←  Backend ← App
 *    Cal ACKs     →  Serial2  →  MQTT publish  →  rainwater/calibration/acks →  mqtt-bridge.js → MongoDB
 *
 *  NOTE: ACKs are written to MongoDB exclusively via the MQTT bridge (mqtt-bridge.js).
 *  The ESP32 does NOT HTTP-POST ACKs directly — the bridge is the single writer,
 *  eliminating the dual-write race on the actuator_states.confirmed field.
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

// Guard: set true while inside the MQTT callback so mqttLog skips the
// re-entrant publish (PubSubClient::publish() overwrites the same internal
// _buffer that the callback's `topic` pointer still references).
static bool mqttCallbackActive = false;

// Print to Serial, WebSerial, and publish raw debug output to rainwater/debug.
// This topic is consumed only by the Serial Monitor — never by the activity log.
// wsLogf is the canonical debug sink; wsLog/wsLogln are thin wrappers around it.
void wsLogf(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print(buf);
    WebSerial.print(buf);
    if (mqttCallbackActive) return;
    // Strip trailing newline — MQTT carries one message per publish
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    if (len > 0 && mqttClient.connected())
        mqttClient.publish("rainwater/debug", buf);
}
template<typename T>
void wsLog(T msg) { wsLogf("%s", String(msg).c_str()); }
template<typename T>
void wsLogln(T msg) { wsLogf("%s\n", String(msg).c_str()); }

// Publish a structured log frame to rainwater/logs.
// Format: "L,<LEVEL>,<CATEGORY>,<message>"
// Only call this for meaningful events — not sensor data or debug noise.
// Never call from within the MQTT callback (mqttCallbackActive guard).
void mqttLog(const char* level, const char* category, const char* message) {
    if (mqttCallbackActive) return;
    if (!mqttClient.connected()) return;
    char frame[220];
    snprintf(frame, sizeof(frame), "L,%s,%s,%s", level, category, message);
    mqttClient.publish("rainwater/logs", frame);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Heartbeat — publish to MQTT so the bridge can upsert device_heartbeats.
//  Previously an HTTP POST to the Vercel backend; moved to MQTT so the ESP32
//  has zero HTTP dependencies and works regardless of the deployment URL.
// ═════════════════════════════════════════════════════════════════════════════
void publishHeartbeat()
{
    if (!mqttClient.connected()) return;
    mqttClient.publish("rainwater/heartbeat", "{\"source\":\"esp32\"}", false);
    wsLogln(F("[MQTT] Heartbeat published"));
}


// ── MQTT ──────────────────────────────────────────────────────────────────────
WiFiClientSecure secureClient;
PubSubClient     mqttClient(secureClient);
unsigned long    lastMqttReconnectMs = 0;

// ── Serial2 link to Arduino Mega ─────────────────────────────────────────────
HardwareSerial MegaSerial(2);   // UART2 — GPIO16 (RX), GPIO17 (TX)

// ── Sensor data store ─────────────────────────────────────────────────────────
StaticJsonDocument<1024> sensorDoc;
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

// ── Recent-command dedup cache ────────────────────────────────────────────────
// QoS-1 broker re-delivery can send the same command twice if a PUBACK is lost
// during an MQTT reconnect. We reject any command that matches one seen within
// the last DEDUP_WINDOW_MS to prevent double-execution on the Mega.
#define DEDUP_SLOTS   4
#define DEDUP_WINDOW_MS 3000
struct DedupEntry { String cmd; unsigned long ts; };
static DedupEntry dedupCache[DEDUP_SLOTS];
static uint8_t    dedupHead = 0;   // ring-buffer write pointer

static bool isDuplicate(const String& cmd)
{
    unsigned long now = millis();
    for (uint8_t i = 0; i < DEDUP_SLOTS; i++) {
        if (dedupCache[i].cmd == cmd && (now - dedupCache[i].ts) < DEDUP_WINDOW_MS)
            return true;
    }
    return false;
}

static void recordSeen(const String& cmd)
{
    dedupCache[dedupHead] = { cmd, millis() };
    dedupHead = (dedupHead + 1) % DEDUP_SLOTS;
}

// ── Timing ────────────────────────────────────────────────────────────────────
unsigned long lastPostMs      = 0;
unsigned long lastHeartbeatMs = 0;

// ── Publish failure tracking ──────────────────────────────────────────────────
// Counts consecutive publish failures. When it hits MQTT_PUBLISH_FAIL_MAX the
// MQTT client is forcibly disconnected so reconnectMQTT() can open a fresh socket.
// Resets to 0 on any successful publish.
static uint8_t publishFailCount = 0;

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
        mqttLog("INFO", "NETWORK", "WiFi connected");
    } else {
        wsLogln("");
        wsLogln(F("[WiFi] FAILED — will retry in loop"));
        mqttLog("WARN", "NETWORK", "WiFi connect failed");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Parse a single frame line from the Mega
//
//  Handled prefixes:
//    S,KEY,VALUE          — sensor reading   → sensorDoc
//    L,LEVEL,CATEGORY,MSG — log entry        → POST /api/activity (source: mega)
//    A,<rest>             — ACK from Mega    → MQTT rainwater/acks → mqtt-bridge.js → MongoDB
//                           (CAL ACKs: A,CAL_* → rainwater/calibration/acks → mqtt-bridge.js)
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
        mqttLog(level.c_str(), category.c_str(), message.c_str());
        return;
    }

    // ── A, ACK frame  ────────────────────────────────────────────────────────
    // Format: A,VALVE,V1,OK  or  A,CAL_PH,C2,MID,OK,2.53
    // ACKs that arrive here are unsolicited (outside a drainCommandQueue window,
    // e.g. pump ACKs that arrive after the 50ms drain window due to the Mega's
    // 100ms pump-start delay). Published to MQTT; the bridge writes MongoDB.
    if (line.startsWith("A,")) {
        wsLog(F("[Mega] ACK: "));
        wsLogln(line);

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
            if (isDuplicate(raw)) {
                wsLogf("[CMD] Dedup — dropping duplicate: %s\n", raw.c_str());
                return;
            }
            recordSeen(raw);
            cmdQueue[cmdQueueLen++] = { raw, isCalibration };
        }
        return;
    }

    JsonArray cmds = doc["commands"].as<JsonArray>();
    if (cmds.size() == 0) { wsLogln(F("[MQTT] commands array empty")); return; }

    for (JsonVariant entry : cmds) {
        if (cmdQueueLen >= CMD_QUEUE_SIZE) break;
        String cmd = entry.as<String>();
        if (cmd.length() == 0) continue;
        if (isDuplicate(cmd)) {
            wsLogf("[CMD] Dedup — dropping duplicate: %s\n", cmd.c_str());
            continue;
        }
        recordSeen(cmd);
        cmdQueue[cmdQueueLen++] = { cmd, isCalibration };
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

    // Non-blocking ACK drain — read one char at a time so millis() is checked
    // between every byte.  readStringUntil('\n') carries a 1-second stream
    // timeout that can silently consume the entire drain window; char-by-char
    // reading avoids that.
    //
    // Window is 200ms (not 50ms) because the Mega's ultrasonic reads block
    // ~25ms per sensor × 5 sensors = ~125ms before comms_receiveCommands()
    // gets to run and send the ACK.  200ms comfortably covers that.
    //
    // OTA safety: this window only runs when cmdQueueLen > 0.  During an OTA
    // flash no actuator commands are queued, so drainCommandQueue() returns
    // immediately and OTA is never blocked.
    char          lineBuf[64];
    uint8_t       lineLen      = 0;
    unsigned long drainUntil   = millis() + 200;
    uint8_t       acksReceived = 0;

    while (millis() < drainUntil && acksReceived < count) {
        if (!MegaSerial.available()) continue;

        char c = (char)MegaSerial.read();
        if (c == '\r') continue;
        if (c != '\n') {
            if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
            continue;
        }
        // '\n' received — process completed line
        if (lineLen == 0) continue;
        lineBuf[lineLen] = '\0';
        lineLen = 0;
        String line = String(lineBuf);

        if (line.startsWith("A,")) {
            wsLogf("[CMD] ACK OK  (%s) → %s\n",
                   cmdQueue[acksReceived].cmd.c_str(), line.c_str());
            const char* ackTopic = cmdQueue[acksReceived].isCalibration
                                   ? MQTT_TOPIC_CAL_ACKS
                                   : MQTT_TOPIC_ACKS;
            mqttClient.publish(ackTopic, line.c_str());
            acksReceived++;
        } else {
            parseMegaLine(line);
        }
    }

    // Report each command that never received an ACK within the drain window.
    // Parse "C,VALVE,V1,OFF" → "Valve V1 OFF — no ACK (50 ms timeout)"
    for (uint8_t i = acksReceived; i < count; i++) {
        const String& raw = cmdQueue[i].cmd;   // e.g. C,VALVE,V1,OFF
        wsLogf("[CMD] TIMEOUT — no ACK for: %s\n", raw.c_str());

        // Extract type / id / state from the command string.
        // Handles both:
        //   3-part: C,VALVE,V1,OFF  → "Valve V1 OFF — no ACK (200ms timeout)"
        //   2-part: C,ESTOP,ON      → "ESTOP ON — no ACK (200ms timeout)"
        int c1 = raw.indexOf(',');             // after 'C'
        int c2 = raw.indexOf(',', c1 + 1);     // after TYPE
        int c3 = raw.indexOf(',', c2 + 1);     // after ID (absent for ESTOP)
        String msg;
        if (c1 > 0 && c2 > 0 && c3 > 0) {
            // C,VALVE,V1,OFF or C,PUMP,P1,ON
            String type  = raw.substring(c1 + 1, c2);
            String id    = raw.substring(c2 + 1, c3);
            String state = raw.substring(c3 + 1);
            type.toLowerCase();
            if (type.length() > 0) type[0] = toupper(type[0]);
            msg = type + " " + id + " " + state + " — no ACK (200ms timeout)";
        } else if (c1 > 0 && c2 > 0) {
            // C,ESTOP,ON  or other 2-part commands
            String type  = raw.substring(c1 + 1, c2);
            String state = raw.substring(c2 + 1);
            msg = type + " " + state + " — no ACK (200ms timeout)";
        } else {
            msg = String("No ACK: ") + raw;
        }
        mqttLog("WARN", "COMMAND", msg.c_str());
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
        mqttLog("INFO", "NETWORK", "MQTT connected");
    } else {
        wsLogf("[MQTT] Failed, rc=%d — will retry in %ds\n",
               mqttClient.state(), MQTT_RECONNECT_MS / 1000);
        mqttLog("WARN", "NETWORK", "MQTT connect failed");
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

    char body[1024];   // must match StaticJsonDocument size — raw fields added ~300 bytes
    serializeJson(sensorDoc, body, sizeof(body));

    if (mqttClient.publish(MQTT_TOPIC_SENSORS, body)) {
        wsLogln(F("[MQTT] Sensors published"));
        publishFailCount = 0;
    } else {
        publishFailCount++;
        wsLogf("[MQTT] Sensor publish failed (%d/%d)\n", publishFailCount, MQTT_PUBLISH_FAIL_MAX);
        if (publishFailCount >= MQTT_PUBLISH_FAIL_MAX) {
            wsLogln(F("[MQTT] Forcing disconnect — dead socket detected"));
            mqttClient.disconnect();
            publishFailCount = 0;
        }
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
    mqttClient.setBufferSize(1024);  // bumped from 512 — raw sensor fields added ~400 bytes to the payload
    mqttClient.setKeepAlive(MQTT_KEEPALIVE_S);
    reconnectMQTT();

    // OTA
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setMdnsEnabled(false);
    ArduinoOTA.onStart([]() {
        wsLogln(F("[OTA] Start"));
        mqttLog("INFO", "SYSTEM", "OTA update started");
    });
    ArduinoOTA.onEnd([]() {
        wsLogln(F("[OTA] Done — rebooting"));
        mqttLog("INFO", "SYSTEM", "OTA update completed");
    });
    ArduinoOTA.onError([](ota_error_t error) {
        wsLog(F("[OTA] Error: "));
        wsLogln(error);
        mqttLog("ERR", "SYSTEM", "OTA update failed");
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
    mqttLog("INFO", "SYSTEM", "ESP32 boot complete");
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
        mqttLog("WARN", "NETWORK", "WiFi disconnected");
        connectWiFi();
    }

    // ── 2. MQTT loop + reconnect ─────────────────────────────────────────────
    if (!mqttClient.loop()) {
        mqttLog("WARN", "NETWORK", "MQTT disconnected");
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

    // ── 5. Heartbeat — lets the dashboard know the ESP32 is alive ────────────
    if ((now - lastHeartbeatMs) >= HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatMs = now;
        publishHeartbeat();
    }
}
