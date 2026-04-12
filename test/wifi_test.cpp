/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  ESP32 WiFi + Serial Test Sketch
 *  RainwaterIOT — Test before connecting to Arduino Mega
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  TESTS:
 *    1. WiFi connection — confirms SSID, password, and IP assignment
 *    2. Serial2 loopback — confirms GPIO16/17 work
 *       (short GPIO16 to GPIO17 with a jumper wire for loopback test)
 *    3. HTTP GET to a public test endpoint — confirms internet + HTTP works
 *
 *  HOW TO RUN:
 *    In platformio.ini, uncomment:
 *      build_src_filter = -<*> +<../test/wifi_test.cpp>
 *    Upload, open Serial Monitor at 115200.
 *    Comment it back out when done.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "config.h"

HardwareSerial MegaSerial(2);   // UART2 — GPIO16 RX, GPIO17 TX

// ── Test 1: WiFi ─────────────────────────────────────────────────────────────
void testWiFi()
{
    Serial.println(F("\n[Test 1] WiFi Connection"));
    Serial.print(F("  Connecting to: "));
    Serial.println(WIFI_SSID);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint8_t attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print('.');
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println();
        Serial.println(F("  PASS — WiFi connected"));
        Serial.print(F("  IP address : ")); Serial.println(WiFi.localIP());
        Serial.print(F("  Signal (RSSI): ")); Serial.print(WiFi.RSSI()); Serial.println(F(" dBm"));
    } else {
        Serial.println();
        Serial.println(F("  FAIL — Could not connect"));
        Serial.println(F("  Check WIFI_SSID and WIFI_PASSWORD in config.h"));
    }
}

// ── Test 2: Serial2 loopback ─────────────────────────────────────────────────
// Short GPIO16 to GPIO17 with a jumper wire before running this test.
void testSerial2Loopback()
{
    Serial.println(F("\n[Test 2] Serial2 Loopback (GPIO16 ↔ GPIO17)"));
    Serial.println(F("  Make sure GPIO16 and GPIO17 are bridged with a jumper wire."));

    const char* testMsg = "RAINWATER_TEST_OK";
    MegaSerial.print(testMsg);
    delay(100);

    String received = "";
    while (MegaSerial.available()) {
        received += (char)MegaSerial.read();
    }

    if (received == testMsg) {
        Serial.println(F("  PASS — Serial2 loopback working"));
    } else if (received.length() == 0) {
        Serial.println(F("  FAIL — Nothing received. Is the jumper wire in place?"));
    } else {
        Serial.print(F("  FAIL — Received unexpected data: "));
        Serial.println(received);
    }
}

// ── Test 3: HTTP connectivity ─────────────────────────────────────────────────
void testHTTP()
{
    Serial.println(F("\n[Test 3] HTTP connectivity"));

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("  SKIP — WiFi not connected"));
        return;
    }

    HTTPClient http;
    // Use httpbin.org as a safe public test endpoint
    http.begin("http://httpbin.org/get");
    int code = http.GET();

    if (code == 200) {
        Serial.println(F("  PASS — HTTP GET returned 200"));
        Serial.println(F("  Internet access confirmed"));
    } else if (code > 0) {
        Serial.print(F("  WARN — HTTP returned code: "));
        Serial.println(code);
    } else {
        Serial.print(F("  FAIL — HTTP error: "));
        Serial.println(http.errorToString(code));
    }
    http.end();
}

// ═════════════════════════════════════════════════════════════════════════════
void setup()
{
    Serial.begin(115200);
    MegaSerial.begin(MEGA_BAUD_RATE, SERIAL_8N1, MEGA_RX_PIN, MEGA_TX_PIN);

    Serial.println(F("=========================================================="));
    Serial.println(F("  RainwaterIOT — ESP32 Test Sketch"));
    Serial.println(F("=========================================================="));

    testWiFi();
    testSerial2Loopback();
    testHTTP();

    Serial.println(F("\n=========================================================="));
    Serial.println(F("  All tests complete. Results above."));
    Serial.println(F("=========================================================="));
}

void loop()
{
    // Nothing — all tests run once in setup()
}
