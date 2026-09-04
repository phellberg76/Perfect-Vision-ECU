/*
 * Raspberry Pi Backup Power and GPS Hat v4 - BLE Bridge Firmware
 *
 * Features:
 *   - GPS: Read NMEA from NEO-M9N, provide data via UART
 *   - BLE: GATT server for Flutter app communication
 *   - WiFi: Station mode with status reporting and provisioning
 *   - UART: Bridge between BLE and RPi backend
 *
 * Hardware:
 *   - ESP32-S3-MINI-1 module
 *   - NEO-M9N GPS on UART1 (GPIO17/18)
 *   - RPi UART0 (GPIO43/44) for commands/responses
 *
 * Communication Flow:
 *   Flutter App <--BLE--> ESP32 <--UART--> RPi Bridge <--HTTP--> Backend
 *
 * UART Messages (ESP32 -> RPi):
 *   {"type":"gps","lat":...,"lng":...,"fix":true,...}
 *   {"type":"wifi","connected":true,"ssid":"...","ip":"...","rssi":-45}
 *   {"type":"heartbeat","uptime":123,...}
 *
 * UART Commands (RPi -> ESP32):
 *   {"cmd":"wifi_status"}
 *   {"cmd":"wifi_config","ssid":"...","password":"..."}
 *   {"cmd":"wifi_disconnect"}
 *   {"cmd":"gps_status"}
 */

#include <Arduino.h>
#include <WiFi.h>
#include <TinyGPSPlus.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "ble_bridge.h"

// =============================================================================
// Pin Definitions
// =============================================================================

// GPS UART1 pins
#define PIN_GPS_TX    17   // To GPS RX
#define PIN_GPS_RX    18   // From GPS TX
#define PIN_LNA_EN    8    // GPS LNA enable

// RPi UART0 pins
#define PIN_RPI_TX    43   // To RPi RX
#define PIN_RPI_RX    44   // From RPi TX

// Status LED
#define PIN_LED       46   // Active HIGH - indicates healthy operation

// =============================================================================
// Constants
// =============================================================================

#define GPS_BAUD           38400
#define RPI_BAUD           115200
#define GPS_UPDATE_MS      1000
#define WIFI_UPDATE_MS     5000
#define HEARTBEAT_MS       5000
#define WIFI_CONNECT_TIMEOUT_MS  15000
#define BLE_DEVICE_NAME    "PerfectPrecisionPlanter"

// WiFi credentials storage key
#define WIFI_PREFS_NAMESPACE  "wifi"

// =============================================================================
// Objects
// =============================================================================

TinyGPSPlus gps;
HardwareSerial GPS_Serial(1);   // UART1 for GPS
HardwareSerial RPI_Serial(0);   // UART0 for RPi

// BLE Bridge
BleBridge bleBridge(RPI_Serial);

// Preferences for WiFi credentials
Preferences preferences;

// GPS State
bool gpsHasFix = false;
unsigned long lastGpsUpdate = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastWifiUpdate = 0;
unsigned long bootTime = 0;

// WiFi State
bool wifiConfigured = false;
bool wifiConnecting = false;
unsigned long wifiConnectStart = 0;
String wifiSsid = "";
String wifiPassword = "";

// UART command buffer
String uartCmdBuffer = "";

// GPS JSON buffer
char gpsJsonBuf[256] = "{\"type\":\"gps\",\"fix\":false}";

// =============================================================================
// Forward Declarations
// =============================================================================

void setupGps();
void processGps();
void buildGpsJson();
void sendGpsToRpi();
void setupWifi();
void processWifi();
void sendWifiStatus();
void loadWifiCredentials();
void saveWifiCredentials(const String& ssid, const String& password);
void connectWifi(const String& ssid, const String& password);
void disconnectWifi();
void processUartCommands();
void handleUartCommand(const String& cmd);
void sendHeartbeat();

// =============================================================================
// Setup
// =============================================================================

void setup() {
    bootTime = millis();

    // Initialize USB Serial for debug output
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n=== ESP32-S3 GPS Hat v4 - BLE Bridge ===");

    // Initialize UART0 for RPi communication
    // Large buffer to handle BLE send delays without overflow
    RPI_Serial.setRxBufferSize(32768);  // 32KB buffer
    RPI_Serial.begin(RPI_BAUD, SERIAL_8N1, PIN_RPI_RX, PIN_RPI_TX);
    delay(100);

    // Send startup message to RPi
    RPI_Serial.println("{\"type\":\"boot\",\"msg\":\"ESP32 BLE Bridge starting...\"}");

    // Initialize status LED (off during init)
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    // Initialize GPS
    Serial.println("[INIT] GPS...");
    RPI_Serial.println("{\"type\":\"init\",\"step\":\"gps\"}");
    setupGps();
    Serial.println("[INIT] GPS OK");

    // Initialize WiFi
    Serial.println("[INIT] WiFi...");
    RPI_Serial.println("{\"type\":\"init\",\"step\":\"wifi\"}");
    setupWifi();
    Serial.println("[INIT] WiFi OK");

    // Initialize BLE
    Serial.println("[INIT] BLE...");
    RPI_Serial.println("{\"type\":\"init\",\"step\":\"ble\"}");
    if (!bleBridge.begin(BLE_DEVICE_NAME)) {
        Serial.println("[INIT] BLE FAILED!");
        RPI_Serial.println("{\"type\":\"error\",\"msg\":\"ble_init_failed\"}");
    } else {
        Serial.println("[INIT] BLE OK");
    }

    // Send startup complete message
    Serial.println("[INIT] Complete!");
    RPI_Serial.println("{\"type\":\"startup\",\"device\":\"rpi-gps-hat-v4\",\"version\":\"3.1.0\",\"mode\":\"ble-bridge\",\"wifi\":true}");

    // Build initial GPS JSON
    buildGpsJson();

    // Turn on status LED - setup complete
    digitalWrite(PIN_LED, HIGH);
    RPI_Serial.println("{\"type\":\"ready\",\"ble\":true}");
}

// =============================================================================
// Main Loop
// =============================================================================

void loop() {
    unsigned long now = millis();

    // Process GPS data
    processGps();

    // Process WiFi connection state
    processWifi();

    // Process UART commands from RPi bridge
    processUartCommands();

    // Update GPS JSON and send to RPi periodically
    if (now - lastGpsUpdate >= GPS_UPDATE_MS) {
        lastGpsUpdate = now;
        buildGpsJson();
        sendGpsToRpi();
    }

    // Send WiFi status periodically
    if (now - lastWifiUpdate >= WIFI_UPDATE_MS) {
        lastWifiUpdate = now;
        sendWifiStatus();
    }

    // Process BLE (handles UART responses too)
    bleBridge.process();

    // Send heartbeat to RPi periodically
    if (now - lastHeartbeat >= HEARTBEAT_MS) {
        lastHeartbeat = now;
        sendHeartbeat();
    }

    // Blink LED based on connection status
    static unsigned long lastBlink = 0;
    if (bleBridge.isConnected()) {
        // Solid on when connected
        digitalWrite(PIN_LED, HIGH);
    } else {
        // Slow blink when advertising
        if (now - lastBlink >= 1000) {
            lastBlink = now;
            digitalWrite(PIN_LED, !digitalRead(PIN_LED));
        }
    }

    // Small delay to prevent tight loop
    delay(1);
}

// =============================================================================
// GPS Functions
// =============================================================================

void setupGps() {
    // Enable GPS LNA
    pinMode(PIN_LNA_EN, OUTPUT);
    digitalWrite(PIN_LNA_EN, HIGH);

    // Initialize GPS serial
    GPS_Serial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
}

void processGps() {
    while (GPS_Serial.available()) {
        char c = GPS_Serial.read();
        gps.encode(c);
    }

    // Update fix status
    gpsHasFix = gps.location.isValid() && gps.location.age() < 2000;
}

void buildGpsJson() {
    char* p = gpsJsonBuf;
    p += sprintf(p, "{\"type\":\"gps\"");

    if (gps.location.isValid()) {
        p += sprintf(p, ",\"lat\":%.7f,\"lng\":%.7f",
            gps.location.lat(), gps.location.lng());
    }

    if (gps.altitude.isValid()) {
        p += sprintf(p, ",\"alt\":%.1f", gps.altitude.meters());
    }

    if (gps.speed.isValid()) {
        p += sprintf(p, ",\"speed\":%.2f", gps.speed.mps());
    }

    if (gps.course.isValid()) {
        p += sprintf(p, ",\"course\":%.1f", gps.course.deg());
    }

    p += sprintf(p, ",\"sats\":%d", gps.satellites.value());

    if (gps.hdop.isValid()) {
        p += sprintf(p, ",\"hdop\":%.2f", gps.hdop.hdop());
    }

    p += sprintf(p, ",\"fix\":%s", gpsHasFix ? "true" : "false");

    if (gps.date.isValid() && gps.time.isValid()) {
        p += sprintf(p, ",\"time\":\"%04d-%02d-%02dT%02d:%02d:%02dZ\"",
            gps.date.year(), gps.date.month(), gps.date.day(),
            gps.time.hour(), gps.time.minute(), gps.time.second());
    }

    sprintf(p, "}");
}

void sendHeartbeat() {
    unsigned long uptime = (millis() - bootTime) / 1000;

    RPI_Serial.printf("{\"type\":\"heartbeat\",\"uptime\":%lu,\"gps_fix\":%s,\"sats\":%d,\"ble_connected\":%s,\"wifi_connected\":%s}\n",
        uptime,
        gpsHasFix ? "true" : "false",
        gps.satellites.value(),
        bleBridge.isConnected() ? "true" : "false",
        WiFi.isConnected() ? "true" : "false");
}

// =============================================================================
// GPS UART Output
// =============================================================================

void sendGpsToRpi() {
    // Send current GPS JSON to RPi
    RPI_Serial.println(gpsJsonBuf);
}

// =============================================================================
// WiFi Functions
// =============================================================================

void setupWifi() {
    // Set WiFi mode to station
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    // Load saved credentials and connect if available
    loadWifiCredentials();

    if (wifiConfigured && wifiSsid.length() > 0) {
        Serial.printf("[WIFI] Connecting to saved network: %s\n", wifiSsid.c_str());
        connectWifi(wifiSsid, wifiPassword);
    } else {
        Serial.println("[WIFI] No saved credentials, waiting for configuration");
    }
}

void loadWifiCredentials() {
    preferences.begin(WIFI_PREFS_NAMESPACE, true);  // Read-only
    wifiSsid = preferences.getString("ssid", "");
    wifiPassword = preferences.getString("password", "");
    preferences.end();

    wifiConfigured = wifiSsid.length() > 0;
    Serial.printf("[WIFI] Loaded credentials: ssid='%s', configured=%s\n",
        wifiSsid.c_str(), wifiConfigured ? "true" : "false");
}

void saveWifiCredentials(const String& ssid, const String& password) {
    preferences.begin(WIFI_PREFS_NAMESPACE, false);  // Read-write
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();

    wifiSsid = ssid;
    wifiPassword = password;
    wifiConfigured = true;

    Serial.printf("[WIFI] Saved credentials: ssid='%s'\n", ssid.c_str());
}

void connectWifi(const String& ssid, const String& password) {
    Serial.printf("[WIFI] Connecting to: %s\n", ssid.c_str());

    WiFi.disconnect();
    delay(100);
    WiFi.begin(ssid.c_str(), password.c_str());

    wifiConnecting = true;
    wifiConnectStart = millis();
}

void disconnectWifi() {
    Serial.println("[WIFI] Disconnecting");
    WiFi.disconnect();
    wifiConnecting = false;
}

void processWifi() {
    // Check for connection timeout
    if (wifiConnecting) {
        if (WiFi.isConnected()) {
            wifiConnecting = false;
            Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

            // Send immediate status update
            sendWifiStatus();
        } else if (millis() - wifiConnectStart > WIFI_CONNECT_TIMEOUT_MS) {
            wifiConnecting = false;
            Serial.println("[WIFI] Connection timeout");

            // Send status showing disconnected
            sendWifiStatus();
        }
    }
}

void sendWifiStatus() {
    StaticJsonDocument<256> doc;
    doc["type"] = "wifi";
    doc["connected"] = WiFi.isConnected();

    if (WiFi.isConnected()) {
        doc["ssid"] = WiFi.SSID();
        doc["ip"] = WiFi.localIP().toString();
        doc["rssi"] = WiFi.RSSI();
    } else if (wifiConfigured) {
        doc["ssid"] = wifiSsid;  // Show configured SSID even when disconnected
    }

    doc["configured"] = wifiConfigured;

    String json;
    serializeJson(doc, json);
    RPI_Serial.println(json);
}

// =============================================================================
// UART Command Processing
// =============================================================================

void processUartCommands() {
    // Read characters from RPi UART into buffer
    while (RPI_Serial.available()) {
        char c = RPI_Serial.read();

        if (c == '\n') {
            // Complete message received
            uartCmdBuffer.trim();
            if (uartCmdBuffer.length() > 0 && uartCmdBuffer.startsWith("{")) {
                handleUartCommand(uartCmdBuffer);
            }
            uartCmdBuffer = "";
        } else {
            uartCmdBuffer += c;

            // Prevent buffer overflow (32KB to handle large HTTP responses)
            if (uartCmdBuffer.length() > 32768) {
                Serial.println("[UART] Buffer overflow, clearing");
                uartCmdBuffer = "";
            }
        }
    }
}

void handleUartCommand(const String& cmdJson) {
    // Quick check: if this doesn't look like a command, forward it to BLE
    // Commands from RPi bridge have "cmd" field, responses have "id" field
    if (cmdJson.indexOf("\"cmd\":") < 0) {
        // Not a command - this is a BLE response from the RPi bridge
        // Forward it to the BLE client
        if (bleBridge.isConnected()) {
            Serial.printf("[UART->BLE] Forwarding response: %d bytes\n", cmdJson.length());
            bleBridge.sendToClient(cmdJson);
        }
        return;
    }

    // Parse the command JSON (small buffer is fine for commands)
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, cmdJson);

    if (error) {
        Serial.printf("[CMD] Parse error: %s\n", error.c_str());
        return;
    }

    const char* cmd = doc["cmd"];
    if (cmd == nullptr) {
        return;
    }

    Serial.printf("[CMD] Received: %s\n", cmd);

    if (strcmp(cmd, "wifi_status") == 0) {
        // Request WiFi status
        sendWifiStatus();
    }
    else if (strcmp(cmd, "wifi_config") == 0) {
        // Configure WiFi
        const char* ssid = doc["ssid"];
        const char* password = doc["password"];

        if (ssid != nullptr) {
            String ssidStr = String(ssid);
            String passStr = password != nullptr ? String(password) : "";

            // Save and connect
            saveWifiCredentials(ssidStr, passStr);
            connectWifi(ssidStr, passStr);

            // Send acknowledgment
            RPI_Serial.println("{\"type\":\"wifi_config_ack\",\"status\":\"connecting\"}");
        } else {
            RPI_Serial.println("{\"type\":\"wifi_config_ack\",\"status\":\"error\",\"msg\":\"ssid_required\"}");
        }
    }
    else if (strcmp(cmd, "wifi_disconnect") == 0) {
        disconnectWifi();
        RPI_Serial.println("{\"type\":\"wifi_config_ack\",\"status\":\"disconnected\"}");
    }
    else if (strcmp(cmd, "gps_status") == 0) {
        // Request GPS status
        sendGpsToRpi();
    }
    else {
        Serial.printf("[CMD] Unknown command: %s\n", cmd);
    }
}
