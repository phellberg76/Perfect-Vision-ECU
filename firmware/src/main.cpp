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
#include <esp_task_wdt.h>
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

// Watchdog pins (RPi GPIO connections)
// For full reset: jumper RPi GPIO6 to GLOBAL_EN on CM4 IO Board J2 pin 1
#define PIN_WDT_HEARTBEAT  3   // ESP32 IO3 <- RPi GPIO5 (heartbeat input, 10k pull-up)
#define PIN_WDT_RESET      4   // ESP32 IO4 -> RPi GPIO6 -> GLOBAL_EN (reset output)

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

// Watchdog settings (for monitoring RPi)
#define WDT_TIMEOUT_MS         30000   // 30 seconds without heartbeat triggers reset
#define WDT_RESET_PULSE_MS     200     // Reset pulse duration
#define WDT_STARTUP_GRACE_MS   60000   // 60 second grace period after boot/reset

// ESP32 hardware watchdog (self-monitoring)
#define ESP32_WDT_TIMEOUT_S    10      // 10 seconds - reset ESP32 if main loop hangs

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

// Watchdog State
volatile unsigned long lastWdtPulse = 0;    // Last time heartbeat changed
volatile bool lastWdtState = false;          // Last heartbeat GPIO state
bool wdtEnabled = true;                      // Watchdog enabled
bool wdtTriggered = false;                   // Has watchdog triggered a reset?
unsigned long wdtResetTime = 0;              // Time of last reset
int wdtResetCount = 0;                       // Number of resets since power on

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
void setupWatchdog();
void processWatchdog();
void triggerReset();
void IRAM_ATTR wdtHeartbeatISR();
void sendEsp32Status();

// =============================================================================
// Setup
// =============================================================================

void setup() {
    // CRITICAL: Set reset pin LOW immediately, then to INPUT
    // External circuit: NPN transistor with 10k pull-down on base
    // GPIO HIGH = transistor ON = GLOBAL_EN LOW = reset (inverted logic)
    // Setting LOW first ensures transistor stays OFF during boot
    pinMode(PIN_WDT_RESET, OUTPUT);
    digitalWrite(PIN_WDT_RESET, LOW);
    pinMode(PIN_WDT_RESET, INPUT);  // 10k external pull-down keeps transistor OFF

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

    // Initialize Watchdog (monitors RPi)
    Serial.println("[INIT] Watchdog...");
    RPI_Serial.println("{\"type\":\"init\",\"step\":\"watchdog\"}");
    setupWatchdog();
    Serial.println("[INIT] Watchdog OK");

    // Initialize ESP32 hardware watchdog (self-monitoring)
    Serial.println("[INIT] ESP32 hardware watchdog...");
    esp_task_wdt_init(ESP32_WDT_TIMEOUT_S, true);  // timeout in seconds, panic on timeout
    esp_task_wdt_add(NULL);  // Add current task (loop task)
    Serial.printf("[INIT] ESP32 WDT OK, timeout=%ds\n", ESP32_WDT_TIMEOUT_S);

    // Send startup complete message
    Serial.println("[INIT] Complete!");
    RPI_Serial.println("{\"type\":\"startup\",\"device\":\"rpi-gps-hat-v4\",\"version\":\"3.4.0\",\"mode\":\"ble-bridge\",\"wifi\":true,\"watchdog\":true}");

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

    // Process watchdog (check for heartbeat timeout)
    processWatchdog();

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

    // Handle BLE admin commands
    if (bleBridge.isRebootRequested()) {
        bleBridge.clearRebootRequest();
        Serial.println("[ADMIN] Reboot RPi requested via BLE");
        delay(100);  // Let BLE ack be sent
        triggerReset();
    }

    if (bleBridge.isStatusRequested()) {
        bleBridge.clearStatusRequest();
        sendEsp32Status();
    }

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

    // Feed ESP32 hardware watchdog
    esp_task_wdt_reset();

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
    unsigned long wdtElapsed = millis() - lastWdtPulse;

    RPI_Serial.printf("{\"type\":\"heartbeat\",\"uptime\":%lu,\"gps_fix\":%s,\"sats\":%d,\"ble_connected\":%s,\"wifi_connected\":%s,\"wdt_enabled\":%s,\"wdt_last_ms\":%lu,\"wdt_resets\":%d}\n",
        uptime,
        gpsHasFix ? "true" : "false",
        gps.satellites.value(),
        bleBridge.isConnected() ? "true" : "false",
        WiFi.isConnected() ? "true" : "false",
        wdtEnabled ? "true" : "false",
        wdtElapsed,
        wdtResetCount);
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
    else if (strcmp(cmd, "wdt_enable") == 0) {
        // Enable watchdog
        wdtEnabled = true;
        lastWdtPulse = millis();  // Reset timer
        RPI_Serial.println("{\"type\":\"wdt_ack\",\"enabled\":true}");
        Serial.println("[WDT] Watchdog enabled");
    }
    else if (strcmp(cmd, "wdt_disable") == 0) {
        // Disable watchdog (for maintenance)
        wdtEnabled = false;
        RPI_Serial.println("{\"type\":\"wdt_ack\",\"enabled\":false}");
        Serial.println("[WDT] Watchdog disabled");
    }
    else if (strcmp(cmd, "wdt_status") == 0) {
        // Report watchdog status
        unsigned long elapsed = millis() - lastWdtPulse;
        RPI_Serial.printf("{\"type\":\"wdt_status\",\"enabled\":%s,\"last_pulse_ms\":%lu,\"timeout_ms\":%d,\"reset_count\":%d}\n",
            wdtEnabled ? "true" : "false",
            elapsed,
            WDT_TIMEOUT_MS,
            wdtResetCount);
    }
    else if (strcmp(cmd, "wdt_pet") == 0) {
        // Software heartbeat (alternative to GPIO)
        lastWdtPulse = millis();
        Serial.println("[WDT] Software heartbeat received");
    }
    else {
        Serial.printf("[CMD] Unknown command: %s\n", cmd);
    }
}

// =============================================================================
// Watchdog Functions
// =============================================================================

// ISR for heartbeat GPIO transitions
void IRAM_ATTR wdtHeartbeatISR() {
    lastWdtPulse = millis();
}

void setupWatchdog() {
    // Configure heartbeat input pin with pull-up
    pinMode(PIN_WDT_HEARTBEAT, INPUT_PULLUP);

    // Configure reset output pin (already set in setup())
    // External NPN transistor with 10k pull-down - INPUT keeps transistor OFF
    pinMode(PIN_WDT_RESET, INPUT);

    // Attach interrupt on any change to heartbeat pin
    attachInterrupt(digitalPinToInterrupt(PIN_WDT_HEARTBEAT), wdtHeartbeatISR, CHANGE);

    // Initialize watchdog timer
    lastWdtPulse = millis();
    lastWdtState = digitalRead(PIN_WDT_HEARTBEAT);
    wdtTriggered = false;
    wdtResetTime = 0;

    Serial.printf("[WDT] Watchdog initialized, timeout=%dms, grace=%dms\n",
        WDT_TIMEOUT_MS, WDT_STARTUP_GRACE_MS);
    Serial.printf("[WDT] Heartbeat pin: IO%d, Reset pin: IO%d\n",
        PIN_WDT_HEARTBEAT, PIN_WDT_RESET);
    Serial.println("[WDT] Reset requires jumper: RPi GPIO6 -> CM4 IO Board J2 GLOBAL_EN");
}

void processWatchdog() {
    if (!wdtEnabled) {
        return;
    }

    unsigned long now = millis();
    unsigned long elapsed = now - lastWdtPulse;
    unsigned long sinceAlert = now - wdtResetTime;
    unsigned long sinceBoot = now - bootTime;

    // Grace period after boot - don't trigger alert while RPi is booting
    if (sinceBoot < WDT_STARTUP_GRACE_MS) {
        return;
    }

    // Grace period after alert - wait for RPi to recover
    if (wdtTriggered && sinceAlert < WDT_STARTUP_GRACE_MS) {
        return;
    }

    // Clear triggered flag after grace period
    if (wdtTriggered && sinceAlert >= WDT_STARTUP_GRACE_MS) {
        wdtTriggered = false;
        Serial.println("[WDT] Grace period ended, monitoring resumed");
    }

    // Check for timeout
    if (elapsed >= WDT_TIMEOUT_MS) {
        Serial.printf("[WDT] TIMEOUT! No heartbeat for %lu ms\n", elapsed);

        // Alert via BLE to connected app
        if (bleBridge.isConnected()) {
            String alert = "{\"type\":\"wdt_reset\",\"msg\":\"RPi not responding, triggering reset\",\"elapsed_ms\":" +
                           String(elapsed) + ",\"reset_count\":" + String(wdtResetCount + 1) + "}";
            bleBridge.sendToClient(alert);
        }

        // Trigger CM4 reset via GLOBAL_EN
        triggerReset();

        // Update state
        wdtTriggered = true;
        wdtResetTime = millis();
        wdtResetCount++;
        lastWdtPulse = millis();  // Reset timer to avoid repeated alerts
    }
}

void triggerReset() {
    // Pull GLOBAL_EN LOW via NPN transistor to reset CM4
    // Circuit: GPIO -> 1k -> Base, 10k pull-down on Base, Collector -> GLOBAL_EN, Emitter -> GND
    // Logic is inverted: GPIO HIGH = transistor ON = GLOBAL_EN LOW = reset
    Serial.println("[WDT] Triggering CM4 reset via GLOBAL_EN...");

    pinMode(PIN_WDT_RESET, OUTPUT);
    digitalWrite(PIN_WDT_RESET, HIGH);  // Turn on transistor, pulls GLOBAL_EN LOW
    delay(WDT_RESET_PULSE_MS);  // Hold for reset
    digitalWrite(PIN_WDT_RESET, LOW);   // Turn off transistor, GLOBAL_EN returns HIGH
    pinMode(PIN_WDT_RESET, INPUT);      // Back to high-Z (10k pull-down keeps transistor OFF)

    Serial.println("[WDT] Reset pulse complete, CM4 should reboot");
}

void sendEsp32Status() {
    // Send ESP32 status to BLE client
    unsigned long uptime = (millis() - bootTime) / 1000;
    unsigned long wdtElapsed = millis() - lastWdtPulse;

    String status = "{\"type\":\"esp32_status\"";
    status += ",\"uptime\":" + String(uptime);
    status += ",\"version\":\"3.4.0\"";
    status += ",\"gps_fix\":" + String(gpsHasFix ? "true" : "false");
    status += ",\"sats\":" + String(gps.satellites.value());
    status += ",\"ble_connected\":" + String(bleBridge.isConnected() ? "true" : "false");
    status += ",\"wifi_connected\":" + String(WiFi.isConnected() ? "true" : "false");
    if (WiFi.isConnected()) {
        status += ",\"wifi_ssid\":\"" + WiFi.SSID() + "\"";
        status += ",\"wifi_rssi\":" + String(WiFi.RSSI());
    }
    status += ",\"wdt_enabled\":" + String(wdtEnabled ? "true" : "false");
    status += ",\"wdt_last_ms\":" + String(wdtElapsed);
    status += ",\"wdt_resets\":" + String(wdtResetCount);
    status += ",\"free_heap\":" + String(ESP.getFreeHeap());
    status += "}";

    bleBridge.sendToClient(status);
    Serial.println("[ADMIN] ESP32 status sent via BLE");
}
