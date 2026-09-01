/*
 * Raspberry Pi Backup Power and GPS Hat v4 - BLE Bridge Firmware
 *
 * Features:
 *   - GPS: Read NMEA from NEO-M9N, provide data via UART
 *   - BLE: GATT server for Flutter app communication
 *   - UART: Bridge between BLE and RPi backend
 *
 * Hardware:
 *   - ESP32-S3-MINI-1 module
 *   - NEO-M9N GPS on UART1 (GPIO17/18)
 *   - RPi UART0 (GPIO43/44) for commands/responses
 *
 * Communication Flow:
 *   Flutter App <--BLE--> ESP32 <--UART--> RPi <--HTTP--> Backend
 */

#include <Arduino.h>
#include <TinyGPSPlus.h>
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
#define HEARTBEAT_MS       5000
#define BLE_DEVICE_NAME    "PerfectPrecisionPlanter"

// =============================================================================
// Objects
// =============================================================================

TinyGPSPlus gps;
HardwareSerial GPS_Serial(1);   // UART1 for GPS
HardwareSerial RPI_Serial(0);   // UART0 for RPi

// BLE Bridge
BleBridge bleBridge(RPI_Serial);

// State
bool gpsHasFix = false;
unsigned long lastGpsUpdate = 0;
unsigned long lastHeartbeat = 0;
unsigned long bootTime = 0;

// GPS JSON buffer
char gpsJsonBuf[256] = "{\"type\":\"gps\",\"fix\":false}";

// =============================================================================
// Forward Declarations
// =============================================================================

void setupGps();
void processGps();
void buildGpsJson();
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
    // Larger buffer to handle BLE send delays without overflow
    RPI_Serial.setRxBufferSize(8192);  // 8KB buffer
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
    RPI_Serial.println("{\"type\":\"startup\",\"device\":\"rpi-gps-hat-v4\",\"version\":\"3.0.0\",\"mode\":\"ble-bridge\"}");

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

    // Update GPS JSON periodically
    if (now - lastGpsUpdate >= GPS_UPDATE_MS) {
        lastGpsUpdate = now;
        buildGpsJson();
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

    RPI_Serial.printf("{\"type\":\"heartbeat\",\"uptime\":%lu,\"gps_fix\":%s,\"sats\":%d,\"ble_connected\":%s}\n",
        uptime,
        gpsHasFix ? "true" : "false",
        gps.satellites.value(),
        bleBridge.isConnected() ? "true" : "false");
}
