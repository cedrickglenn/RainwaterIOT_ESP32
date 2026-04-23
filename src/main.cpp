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
 *  DUAL-CORE ARCHITECTURE:
 *    Core 0 (serialTask) — serial RX from Mega only.
 *      Reads bytes, assembles lines, calls parseMegaLine().
 *      parseMegaLine() NEVER calls mqttClient directly — ACKs are pushed to
 *      ackQueue under dataMutex so core 1 publishes them.
 *
 *    Core 1 (Arduino loop()) — all MQTT I/O and serial TX.
 *      mqttClient.loop(), reconnect, publish sensors/actuators/heartbeat/ACKs,
 *      drain incoming command queue to Mega via MegaSerial.println(), OTA.
 *      Arduino framework runs loop() on core 1 by default.
 *
 *    dataMutex protects every shared variable between the two cores:
 *      sensorDoc, sensorDataReady, pendingActuators, actuatorsReadyToSend,
 *      ackQueue/ackQueueLen.
 *
 *    LOCK DISCIPLINE — always: take → touch shared state → release.
 *    Never call mqttClient while holding dataMutex.
 *    Never hold dataMutex across a yield or delay.
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

// ── Dual-core mutex ───────────────────────────────────────────────────────────
// Protects all shared state between serialTask (core 1) and mqttTask (core 0).
// LOCK DISCIPLINE: take → touch shared state → release.
// Never call mqttClient while holding this mutex.
// Never hold across yield() or delay().
static SemaphoreHandle_t dataMutex = nullptr;

// ── Serial line buffer — owned exclusively by serialTask (core 1) ─────────────
// Never touched by core 0. No mutex needed for these.
static char    megaLineBuf[128];
static uint8_t megaLineLen = 0;

// ── ACK queue — written by core 1 (parseMegaLine), drained by core 0 ─────────
// Core 1 pushes ACKs here instead of calling mqttClient.publish() directly.
// Core 0 drains this queue after mqttClient.loop() returns.
// Protected by dataMutex.
#define ACK_QUEUE_SIZE 8
struct QueuedAck {
    char    topic[64];
    char    payload[768];  // large enough for one full Mega telemetry cycle batch (~640 bytes)
};
static QueuedAck ackQueue[ACK_QUEUE_SIZE];
static uint8_t   ackQueueLen = 0;

// ── wsLogf — safe to call from either core ───────────────────────────────────
// Outputs to Serial + WebSerial from either core.
// MQTT publish to rainwater/debug only from core 1 (loop) — mqttClient is not thread-safe.
// Core 0 (serialTask) gets Serial + WebSerial only; S, sensor lines are batched
// per-cycle in parseMegaLine and published as a single message on STATE.
void wsLogf(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print(buf);
    WebSerial.print(buf);

    // Only publish to rainwater/debug from core 1 — mqttClient is not thread-safe.
    if (xPortGetCoreID() != 1) return;
    if (mqttCallbackActive) return;
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
// Only call from core 0 — mqttClient is not thread-safe.
void mqttLog(const char* level, const char* category, const char* message) {
    if (xPortGetCoreID() != 0) return;
    if (mqttCallbackActive) return;
    if (!mqttClient.connected()) return;
    char frame[220];
    snprintf(frame, sizeof(frame), "L,%s,%s,%s", level, category, message);
    mqttClient.publish("rainwater/logs", frame);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Heartbeat
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

// ── Shared sensor state — protected by dataMutex ─────────────────────────────
StaticJsonDocument<3072> sensorDoc;
bool   sensorDataReady      = false;
String pendingActuators     = "";
bool   actuatorsReadyToSend = false;

// ── Serial RX ownership flag ──────────────────────────────────────────────────
// drainCommandQueue() (core 1) needs exclusive MegaSerial.read() access during
// its 200ms ACK window. serialTask (core 0) checks this flag and yields while
// it is set. Written only by core 1, read by core 0 — volatile is sufficient
// (no mutex needed; single writer, single reader, bool write is atomic on ESP32).
static volatile bool serialDraining = false;

// ── Incoming command queue — written by MQTT callback (core 1), drained by core 1
// No mutex needed — both sides run on core 1.
#define CMD_QUEUE_SIZE 8
struct QueuedCmd {
    String  cmd;
    bool    isCalibration;
};
QueuedCmd cmdQueue[CMD_QUEUE_SIZE];
uint8_t   cmdQueueLen = 0;

// ── Recent-command dedup cache ────────────────────────────────────────────────
#define DEDUP_SLOTS   4
#define DEDUP_WINDOW_MS 3000
struct DedupEntry { String cmd; unsigned long ts; };
static DedupEntry dedupCache[DEDUP_SLOTS];
static uint8_t    dedupHead = 0;

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
static uint8_t publishFailCount  = 0;
static uint8_t reconnectFailCount = 0;

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
//  Parse a single frame line from the Mega — runs on core 1 (serialTask)
//
//  IMPORTANT: Never calls mqttClient directly.
//  ACKs are pushed to ackQueue (under dataMutex) for core 0 to publish.
//  sensorDoc writes are under dataMutex.
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

        // Accumulate key=value pairs into a per-cycle debug buffer.
        // Flushed as a single MQTT publish when STATE arrives (last line of each cycle).
        // This replaces 32 individual wsLogf() calls (one per key) with one publish per cycle,
        // preventing the MQTT burst that caused EMQX to batch all lines with the same timestamp.
        static String megaDebugBuf = "";

        // Always output per-line to Serial + WebSerial for local visibility.
        Serial.print(F("[Mega] ")); Serial.print(key); Serial.print(F(" = ")); Serial.println(value);
        WebSerial.print(F("[Mega] ")); WebSerial.print(key); WebSerial.print(F(" = ")); WebSerial.println(value);

        // Take mutex only for the shared state write — release immediately after.
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        if (key == "STATE") {
            sensorDoc["ff_state"]       = value.substring(0, value.indexOf(',')).toInt();
            int s2 = value.indexOf(',') + 1;
            int s3 = value.indexOf(',', s2);
            sensorDoc["filter_mode"]    = value.substring(s2, s3).toInt();
            sensorDoc["backwash_state"] = value.substring(s3 + 1).toInt();
            sensorDataReady = true;
        } else if (key == "ACTUATORS") {
            pendingActuators = value;
        } else {
            sensorDoc[key] = value.toFloat();
        }
        xSemaphoreGive(dataMutex);

        if (key == "STATE") {
            // STATE is always the last line of the Mega telemetry cycle (comms.cpp).
            // Push the accumulated debug buffer as one queue item — Core 1 publishes it
            // via drainAckQueue(). One item per 2s cycle keeps the queue clear.
            // Format: "MEGA|KEY=VALUE|KEY=VALUE|...|STATE=0,1,0"
            // The MEGA prefix lets the dashboard detect and render this as a sensor table.
            megaDebugBuf += "|STATE=" + value;
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            if (ackQueueLen < ACK_QUEUE_SIZE) {
                strncpy(ackQueue[ackQueueLen].topic,   "rainwater/debug",        sizeof(ackQueue[0].topic) - 1);
                strncpy(ackQueue[ackQueueLen].payload, megaDebugBuf.c_str(),     sizeof(ackQueue[0].payload) - 1);
                ackQueue[ackQueueLen].topic[sizeof(ackQueue[0].topic) - 1]   = '\0';
                ackQueue[ackQueueLen].payload[sizeof(ackQueue[0].payload) - 1] = '\0';
                ackQueueLen++;
            }
            xSemaphoreGive(dataMutex);
            megaDebugBuf = "";
        } else {
            // Pipe-separated: "MEGA|KEY=VALUE|KEY=VALUE|..."
            if (megaDebugBuf.length() == 0) megaDebugBuf = "MEGA";
            megaDebugBuf += "|" + key + "=" + value;
        }
        return;
    }

    // ── L, log frame ─────────────────────────────────────────────────────────
    // Log to serial/WebSerial only — mqttLog is core 0 only.
    if (line.startsWith("L,")) {
        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        int c3 = line.indexOf(',', c2 + 1);
        if (c3 < 0) return;

        String level    = line.substring(c1 + 1, c2);
        String category = line.substring(c2 + 1, c3);
        String message  = line.substring(c3 + 1);
        message.trim();

        // wsLogf is safe from core 1 (skips MQTT publish, outputs Serial+WebSerial).
        wsLogf("[Mega/%s] %s: %s\n", category.c_str(), level.c_str(), message.c_str());

        // Queue a structured log publish for core 0.
        // Reuse the ACK queue with the logs topic — same mechanism, same drain path.
        char frame[220];
        snprintf(frame, sizeof(frame), "L,%s,%s,%s", level.c_str(), category.c_str(), message.c_str());

        xSemaphoreTake(dataMutex, portMAX_DELAY);
        if (ackQueueLen < ACK_QUEUE_SIZE) {
            strncpy(ackQueue[ackQueueLen].topic,   "rainwater/logs", sizeof(ackQueue[0].topic) - 1);
            strncpy(ackQueue[ackQueueLen].payload, frame,            sizeof(ackQueue[0].payload) - 1);
            ackQueue[ackQueueLen].topic[sizeof(ackQueue[0].topic) - 1]     = '\0';
            ackQueue[ackQueueLen].payload[sizeof(ackQueue[0].payload) - 1] = '\0';
            ackQueueLen++;
        }
        xSemaphoreGive(dataMutex);
        return;
    }

    // ── A, ACK frame ─────────────────────────────────────────────────────────
    // Push to ackQueue — core 0 publishes it. Never call mqttClient here.
    if (line.startsWith("A,")) {
        wsLog(F("[Mega] ACK: "));
        wsLogln(line);

        const char* topic = line.startsWith("A,CAL_")
                            ? MQTT_TOPIC_CAL_ACKS
                            : MQTT_TOPIC_ACKS;

        xSemaphoreTake(dataMutex, portMAX_DELAY);
        if (ackQueueLen < ACK_QUEUE_SIZE) {
            strncpy(ackQueue[ackQueueLen].topic,   topic,           sizeof(ackQueue[0].topic) - 1);
            strncpy(ackQueue[ackQueueLen].payload, line.c_str(),    sizeof(ackQueue[0].payload) - 1);
            ackQueue[ackQueueLen].topic[sizeof(ackQueue[0].topic) - 1]     = '\0';
            ackQueue[ackQueueLen].payload[sizeof(ackQueue[0].payload) - 1] = '\0';
            ackQueueLen++;
        }
        xSemaphoreGive(dataMutex);
        return;
    }

    // ── C, command echo ───────────────────────────────────────────────────────
    if (line.startsWith("C,")) {
        wsLog(F("[Mega] Echo: "));
        wsLogln(line);
        return;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Enqueue commands from a raw payload string — runs on core 0 (MQTT callback)
//  No mutex needed — cmdQueue is only accessed on core 0.
// ═════════════════════════════════════════════════════════════════════════════
void enqueuePayload(const char* buf, bool isCalibration)
{
    JsonDocument doc;
    DeserializationError jsonErr = deserializeJson(doc, buf);

    if (jsonErr != DeserializationError::Ok) {
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
//  MQTT — incoming message callback — runs on core 0
//  Only enqueues — no mqttClient publish/subscribe calls allowed here.
// ═════════════════════════════════════════════════════════════════════════════
void onMqttMessage(char* topic, byte* payload, unsigned int length)
{
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
//  Drain ACK queue — runs on core 0 after mqttClient.loop()
//  Takes mutex to snapshot the queue, releases before publishing.
//  Lock discipline: take → copy → release → publish (never publish under lock).
// ═════════════════════════════════════════════════════════════════════════════
void drainAckQueue()
{
    // Publish ONE item per loop() tick so each message gets its own mqttClient.loop()
    // flush on the next iteration. Publishing all items in a tight loop sends them as
    // a single TCP burst; EMQX batches that burst and delivers it to subscribers at
    // once, causing serial-monitor log lines to appear with identical timestamps.
    // One-per-tick spreads messages across individual TCP frames at ~1 ms apart.
    if (!mqttClient.connected()) return;

    QueuedAck item;
    bool      hasItem = false;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (ackQueueLen > 0) {
        item = ackQueue[0];
        for (uint8_t i = 1; i < ackQueueLen; i++) {
            ackQueue[i - 1] = ackQueue[i];
        }
        ackQueueLen--;
        hasItem = true;
    }
    xSemaphoreGive(dataMutex);
    // Mutex released — safe to call mqttClient now.

    if (hasItem) {
        mqttClient.publish(item.topic, item.payload, false);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Drain command queue — runs on core 0
//  Sends commands to Mega via serial TX, collects ACKs within a drain window.
//  Serial TX is core 0 only. Serial RX during the drain window is safe because
//  serialTask (core 1) is still running and will process any bytes that arrive
//  outside the drain window — the drain window only reads directly to collect
//  ACKs that arrive synchronously in response to the commands just sent.
// ═════════════════════════════════════════════════════════════════════════════
void drainCommandQueue()
{
    if (cmdQueueLen == 0) return;

    uint8_t count = cmdQueueLen;
    cmdQueueLen = 0;

    for (uint8_t i = 0; i < count; i++) {
        wsLogf("[CMD] -> Mega: %s\n", cmdQueue[i].cmd.c_str());
        MegaSerial.println(cmdQueue[i].cmd);
    }

    // 200ms ACK drain window — collect synchronous ACKs from Mega.
    // serialDraining tells serialTask to yield so only this core reads MegaSerial.
    serialDraining = true;
    char          lineBuf[128];
    uint8_t       lineLen      = 0;
    unsigned long drainUntil   = millis() + 200;
    uint8_t       acksReceived = 0;

    while (millis() < drainUntil && acksReceived < count) {
        if (!MegaSerial.available()) continue;

        char c = (char)MegaSerial.read();
        if (c == '\r') continue;
        if (c != '\n') {
            if (lineLen < sizeof(lineBuf) - 1) {
                lineBuf[lineLen++] = c;
            } else {
                lineBuf[sizeof(lineBuf) - 1] = '\0';
                char msg[80];
                snprintf(msg, sizeof(msg), "drain RX overflow: %.40s...", lineBuf);
                mqttLog("WARN", "SERIAL", msg);
                lineLen = 0;
            }
            continue;
        }
        if (lineLen == 0) continue;
        lineBuf[lineLen] = '\0';
        lineLen = 0;
        String line = String(lineBuf);

        if (line.startsWith("A,")) {
            wsLogf("[CMD] ACK (%s) → %s\n", cmdQueue[acksReceived].cmd.c_str(), line.c_str());
            const char* ackTopic = cmdQueue[acksReceived].isCalibration
                                   ? MQTT_TOPIC_CAL_ACKS
                                   : MQTT_TOPIC_ACKS;
            if (mqttClient.connected())
                mqttClient.publish(ackTopic, line.c_str());
            acksReceived++;
        } else {
            // Non-ACK line received during drain window — parse it normally.
            // parseMegaLine is safe to call from core 0 since it only touches
            // shared state under dataMutex and never calls mqttClient.
            parseMegaLine(line);
        }
    }

    serialDraining = false;  // release serial RX back to serialTask

    for (uint8_t i = acksReceived; i < count; i++) {
        const String& raw = cmdQueue[i].cmd;
        wsLogf("[CMD] TIMEOUT — no ACK for: %s\n", raw.c_str());

        int c1 = raw.indexOf(',');
        int c2 = raw.indexOf(',', c1 + 1);
        int c3 = raw.indexOf(',', c2 + 1);
        String msg;
        if (c1 > 0 && c2 > 0 && c3 > 0) {
            String type  = raw.substring(c1 + 1, c2);
            String id    = raw.substring(c2 + 1, c3);
            String state = raw.substring(c3 + 1);
            type.toLowerCase();
            if (type.length() > 0) type[0] = toupper(type[0]);
            msg = type + " " + id + " " + state + " — no ACK (200ms timeout)";
        } else if (c1 > 0 && c2 > 0) {
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
//  MQTT reconnect — core 0 only
// ═════════════════════════════════════════════════════════════════════════════
void reconnectMQTT()
{
    if (mqttClient.connected()) return;
    if (millis() - lastMqttReconnectMs < MQTT_RECONNECT_MS) return;
    lastMqttReconnectMs = millis();

    wsLogf("[MQTT] Connecting to %s...\n", MQTT_BROKER);
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD,
                           nullptr, 0, false, nullptr, true)) {  // cleanSession=true — discard broker-queued commands on reconnect
        wsLogln(F("[MQTT] Connected"));
        reconnectFailCount = 0;
        mqttClient.subscribe(MQTT_TOPIC_COMMANDS);
        mqttClient.subscribe(MQTT_TOPIC_CAL_COMMANDS);
        wsLogln(F("[MQTT] Subscribed to commands + calibration/commands"));
        mqttLog("INFO", "NETWORK", "MQTT connected");
    } else {
        reconnectFailCount++;
        wsLogf("[MQTT] Failed, rc=%d (%d/%d) — will retry in %ds\n",
               mqttClient.state(), reconnectFailCount, MQTT_RECONNECT_FAIL_MAX,
               MQTT_RECONNECT_MS / 1000);
        mqttLog("WARN", "NETWORK", "MQTT connect failed");

        if (reconnectFailCount >= MQTT_RECONNECT_FAIL_MAX) {
            wsLogln(F("[MQTT] Too many failures — recycling WiFi"));
            mqttLog("WARN", "NETWORK", "Recycling WiFi — stale TCP sockets");
            reconnectFailCount = 0;
            WiFi.disconnect(true);
            delay(500);
            connectWiFi();
            lastMqttReconnectMs = millis() - MQTT_RECONNECT_MS + MQTT_SOCKET_COOLDOWN_MS;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Publish sensor data — core 0 only
//  Takes mutex to snapshot shared state, releases before publishing.
//  Lock discipline: take → copy → release → publish.
// ═════════════════════════════════════════════════════════════════════════════
void publishSensorData()
{
    // Snapshot shared state under lock.
    char   body[3072];
    bool   hasActuators     = false;
    String actuatorsSnapshot = "";

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (!sensorDataReady) {
        xSemaphoreGive(dataMutex);
        return;
    }
    sensorDoc["uptime_ms"] = millis();
    size_t bodyLen = serializeJson(sensorDoc, body, sizeof(body));
    sensorDataReady = false;
    sensorDoc.clear();
    if (pendingActuators.length() > 0) {
        hasActuators      = true;
        actuatorsSnapshot = pendingActuators;
    }
    xSemaphoreGive(dataMutex);
    // Mutex released — safe to call wsLogf (takes dataMutex internally) and mqttClient now.
    wsLogf("[MQTT] Payload size: %u bytes (buffer: %u)\n", (unsigned)bodyLen, (unsigned)mqttClient.getBufferSize());

    if (!mqttClient.connected()) return;

    if (mqttClient.publish(MQTT_TOPIC_SENSORS, body)) {
        wsLogln(F("[MQTT] Sensors published"));
        publishFailCount = 0;

        // Deferred actuator publish — after mqttClient.loop() has flushed the
        // sensor publish buffer. Signal via actuatorsReadyToSend (core 0 only,
        // no mutex needed) so the next loop iteration publishes it.
        if (hasActuators) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            actuatorsReadyToSend = true;
            xSemaphoreGive(dataMutex);
        }
    } else {
        publishFailCount++;
        wsLogf("[MQTT] Sensor publish failed (%d/%d)\n", publishFailCount, MQTT_PUBLISH_FAIL_MAX);
        if (publishFailCount >= MQTT_PUBLISH_FAIL_MAX) {
            wsLogln(F("[MQTT] Forcing disconnect — dead socket detected"));
            mqttClient.disconnect();
            secureClient.stop();
            lastMqttReconnectMs = millis() - MQTT_RECONNECT_MS + MQTT_SOCKET_COOLDOWN_MS;
            publishFailCount = 0;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Serial task — core 1
//  Reads Mega serial RX continuously, assembles lines, calls parseMegaLine().
//  Never calls mqttClient. Never does serial TX.
// ═════════════════════════════════════════════════════════════════════════════
void serialTask(void* /*param*/)
{
    static size_t   rxPeak        = 0;
    static uint32_t lastPeakLogMs = 0;

    for (;;) {
        // Yield while drainCommandQueue() owns MegaSerial RX.
        // The drain window is at most 200ms — serialTask resumes immediately after.
        if (serialDraining) {
            vTaskDelay(1 / portTICK_PERIOD_MS);
            continue;
        }

        uint32_t now = millis();

        size_t avail = MegaSerial.available();
        if (avail > rxPeak) rxPeak = avail;

        if ((now - lastPeakLogMs) >= 30000) {
            lastPeakLogMs = now;
            wsLogf("[Serial] RX peak in last 30s: %u bytes (buf 1024)\n", rxPeak);
            rxPeak = 0;
        }

        while (MegaSerial.available()) {
            char c = (char)MegaSerial.read();
            if (c == '\r') continue;
            if (c == '\n') {
                if (megaLineLen > 0) {
                    megaLineBuf[megaLineLen] = '\0';
                    parseMegaLine(String(megaLineBuf));
                    megaLineLen = 0;
                }
            } else {
                if (megaLineLen < sizeof(megaLineBuf) - 1) {
                    megaLineBuf[megaLineLen++] = c;
                } else {
                    megaLineBuf[sizeof(megaLineBuf) - 1] = '\0';
                    char msg[80];
                    snprintf(msg, sizeof(msg), "RX line overflow: %.40s...", megaLineBuf);
                    // Can't call mqttLog from core 1 — wsLogf only.
                    wsLogf("[SERIAL] WARN: %s\n", msg);
                    megaLineLen = 0;
                }
            }
        }

        // Yield to avoid starving other core 1 tasks (watchdog).
        vTaskDelay(1 / portTICK_PERIOD_MS);
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

    // Create mutex before starting any tasks or initialising MQTT.
    dataMutex = xSemaphoreCreateMutex();
    configASSERT(dataMutex);

    MegaSerial.setRxBufferSize(1024);
    MegaSerial.begin(MEGA_BAUD_RATE, SERIAL_8N1, MEGA_RX_PIN, MEGA_TX_PIN);
    Serial.println(F("[Init] Serial2 (Mega link) started, RX buffer 1024 bytes"));

    connectWiFi();

    secureClient.setInsecure();

    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(onMqttMessage);
    mqttClient.setBufferSize(3072);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE_S);
    reconnectMQTT();

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

    WebSerial.begin(&wsServer);
    wsServer.begin();
    Serial.print(F("[Init] WebSerial at http://"));
    Serial.print(WiFi.localIP());
    Serial.println(F("/webserial"));

    // Pin serial task to core 1, stack 4096 bytes, priority 1.
    // MQTT loop (Arduino loop()) runs on core 1 by default in Arduino framework,
    // but we pin it to core 0 via the task below and leave loop() as the idle
    // fallback. Alternatively we simply let loop() run on whichever core Arduino
    // assigns (core 1) and pin serialTask to core 0 — but keeping MQTT on core 0
    // matches ESP32 convention (core 0 = protocol stack, core 1 = app).
    // Here: serialTask on core 0, loop() stays on core 1 (Arduino default).
    // This keeps the MQTT/network stack on core 0 where WiFi runs.
    xTaskCreatePinnedToCore(
        serialTask,
        "serialTask",
        4096,
        nullptr,
        1,           // priority 1 — same as loop(), yields every 1ms
        nullptr,
        0            // core 0
    );

    wsLogln(F("=========================================================="));
    wsLogln(F("  ESP32 Ready"));
    wsLogln(F("=========================================================="));
    mqttLog("INFO", "SYSTEM", "ESP32 boot complete");
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP — runs on core 1 (Arduino framework default)
//  Owns all mqttClient calls, serial TX, and OTA.
//  serialTask runs on core 0 and owns serial RX.
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

    // ── 2b. Drain ACK queue (ACKs + logs queued by serialTask on core 0) ─────
    drainAckQueue();

    // ── 2c. Drain incoming command queue → Mega serial TX ────────────────────
    drainCommandQueue();

    // ── 2d. Deferred actuator publish ────────────────────────────────────────
    {
        bool shouldSend = false;
        String actuatorsSnapshot = "";

        xSemaphoreTake(dataMutex, portMAX_DELAY);
        if (actuatorsReadyToSend) {
            shouldSend           = true;
            actuatorsSnapshot    = pendingActuators;
            actuatorsReadyToSend = false;
            pendingActuators     = "";
        }
        xSemaphoreGive(dataMutex);
        // Mutex released before publish.

        if (shouldSend && mqttClient.connected()) {
            mqttClient.publish("rainwater/actuators", actuatorsSnapshot.c_str(), false);
        }
    }

    // ── 3. Publish sensor data ───────────────────────────────────────────────
    {
        bool ready = false;
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        ready = sensorDataReady;
        xSemaphoreGive(dataMutex);

        if (ready && (now - lastPostMs) >= POST_INTERVAL_MS) {
            lastPostMs = now;
            publishSensorData();
        }
    }

    // ── 4. Heartbeat ─────────────────────────────────────────────────────────
    if ((now - lastHeartbeatMs) >= HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatMs = now;
        publishHeartbeat();
    }
}
