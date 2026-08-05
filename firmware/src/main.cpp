/*
 * Raspberry Pi Backup Power and GPS Hat v4 - ESP32-S3 Firmware
 *
 * Purpose:
 *   - Decode NMEA from NEO-M9N GPS and send JSON to RPi
 *   - Provide watchdog/heartbeat messaging
 *   - Support sleep/wake commands from RPi
 *   - Keep SPI pins high-Z to not interfere with CAN controllers
 *
 * Hardware:
 *   - ESP32-S3-MINI-1 module
 *   - NEO-M9N GPS module on UART1 (TXD1/RXD1)
 *   - RPi connected via UART0 (TXD0/RXD0 = GPIO43/44)
 *
 * JSON Messages to RPi:
 *   {"type":"gps","lat":59.123,"lng":18.456,"alt":45.2,"speed":12.5,"course":180.0,"sats":8,"hdop":1.2,"fix":true,"time":"2026-08-05T09:45:00Z"}
 *   {"type":"heartbeat","uptime":1234,"gps_ok":true,"sats":8}
 *   {"type":"ack","cmd":"sleep","status":"ok"}
 *
 * Commands from RPi:
 *   {"cmd":"status"}     - Request status
 *   {"cmd":"sleep"}      - Enter deep sleep
 *   {"cmd":"wake"}       - Configure wake sources
 *   {"cmd":"gps_rate",value":1000}  - Set GPS update rate in ms
 *   PING                 - Simple watchdog ping (responds with PONG)
 */

#include <Arduino.h>
#include <TinyGPSPlus.h>

// Pin Definitions - SPI (set as inputs - shared bus with RPi)
#define PIN_SPI_MOSI  11
#define PIN_SPI_MISO  13
#define PIN_SPI_SCK   12
#define PIN_SPI_CS    4

// Pin Definitions - GPS (UART1)
#define PIN_GPS_TX    17
#define PIN_GPS_RX    18
#define PIN_LNA_EN    8

// Pin Definitions - RPi UART0
#define PIN_RPI_TX    43
#define PIN_RPI_RX    44

// Timing
#define GPS_UPDATE_INTERVAL   1000   // Send GPS JSON every 1 second
#define HEARTBEAT_INTERVAL    5000   // Send heartbeat every 5 seconds
#define WATCHDOG_TIMEOUT      30000  // Watchdog timeout 30 seconds

// Serial settings
#define GPS_BAUD      38400
#define RPI_BAUD      115200

// Objects
TinyGPSPlus gps;
HardwareSerial GPS_Serial(1);
HardwareSerial RPI_Serial(0);

// Timing
unsigned long lastGpsUpdate = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastRpiPing = 0;
unsigned long gpsUpdateInterval = GPS_UPDATE_INTERVAL;

// State
bool gpsHasFix = false;
bool rpiWatchdogEnabled = false;

// Forward declarations
void processGPS();
void processRPiCommands();
void sendGpsJson();
void sendHeartbeat();
void sendAck(const char* cmd, const char* status);
void sendError(const char* msg);
void handleCommand(String& cmd);
void enterSleep(int seconds);

void setup() {
    // CRITICAL: Set SPI pins as inputs to avoid bus contention
    pinMode(PIN_SPI_MOSI, INPUT);
    pinMode(PIN_SPI_MISO, INPUT);
    pinMode(PIN_SPI_SCK, INPUT);
    pinMode(PIN_SPI_CS, INPUT);

    // Initialize UART0 for RPi communication
    RPI_Serial.begin(RPI_BAUD, SERIAL_8N1, PIN_RPI_RX, PIN_RPI_TX);

    // Initialize GPS LNA
    pinMode(PIN_LNA_EN, OUTPUT);
    digitalWrite(PIN_LNA_EN, HIGH);

    // Initialize GPS serial
    GPS_Serial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

    // Startup message
    RPI_Serial.println("{\"type\":\"startup\",\"device\":\"rpi-gps-hat-v4\",\"version\":\"1.0.0\"}");
}

void loop() {
    // Read GPS data
    processGPS();

    // Process commands from RPi
    processRPiCommands();

    // Send GPS update at interval
    unsigned long now = millis();
    if (now - lastGpsUpdate >= gpsUpdateInterval) {
        lastGpsUpdate = now;
        sendGpsJson();
    }

    // Send heartbeat
    if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
        lastHeartbeat = now;
        sendHeartbeat();
    }

    // Check watchdog (if enabled)
    if (rpiWatchdogEnabled && (now - lastRpiPing > WATCHDOG_TIMEOUT)) {
        // RPi hasn't pinged - could trigger alert or action
        RPI_Serial.println("{\"type\":\"warning\",\"msg\":\"watchdog_timeout\"}");
        lastRpiPing = now; // Reset to avoid spam
    }
}

void processGPS() {
    while (GPS_Serial.available()) {
        char c = GPS_Serial.read();
        gps.encode(c);
    }

    // Update fix status
    gpsHasFix = gps.location.isValid() && gps.location.age() < 2000;
}

void processRPiCommands() {
    if (!RPI_Serial.available()) return;

    String cmd = RPI_Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.length() == 0) return;

    // Simple PING/PONG for watchdog
    if (cmd == "PING") {
        RPI_Serial.println("PONG");
        lastRpiPing = millis();
        return;
    }

    // Handle JSON or simple commands
    handleCommand(cmd);
}

void handleCommand(String& cmd) {
    lastRpiPing = millis();  // Any command counts as activity

    if (cmd == "STATUS" || cmd == "{\"cmd\":\"status\"}") {
        sendHeartbeat();
    }
    else if (cmd == "SLEEP" || cmd.startsWith("{\"cmd\":\"sleep\"")) {
        // Extract sleep duration if provided
        int seconds = 0;
        int idx = cmd.indexOf("\"seconds\":");
        if (idx > 0) {
            seconds = cmd.substring(idx + 10).toInt();
        }
        sendAck("sleep", "entering");
        delay(100);
        enterSleep(seconds);
    }
    else if (cmd.startsWith("{\"cmd\":\"gps_rate\"")) {
        int idx = cmd.indexOf("\"value\":");
        if (idx > 0) {
            gpsUpdateInterval = cmd.substring(idx + 8).toInt();
            if (gpsUpdateInterval < 100) gpsUpdateInterval = 100;
            if (gpsUpdateInterval > 60000) gpsUpdateInterval = 60000;
            sendAck("gps_rate", "ok");
        } else {
            sendError("missing value");
        }
    }
    else if (cmd == "WATCHDOG_ON" || cmd == "{\"cmd\":\"watchdog\",\"enable\":true}") {
        rpiWatchdogEnabled = true;
        lastRpiPing = millis();
        sendAck("watchdog", "enabled");
    }
    else if (cmd == "WATCHDOG_OFF" || cmd == "{\"cmd\":\"watchdog\",\"enable\":false}") {
        rpiWatchdogEnabled = false;
        sendAck("watchdog", "disabled");
    }
    else if (cmd == "RESET") {
        sendAck("reset", "restarting");
        delay(100);
        ESP.restart();
    }
    else if (cmd == "GPS_RAW_ON") {
        // Enable raw NMEA passthrough for debugging
        RPI_Serial.println("{\"type\":\"mode\",\"raw\":true}");
        while (true) {
            if (GPS_Serial.available()) {
                RPI_Serial.write(GPS_Serial.read());
            }
            if (RPI_Serial.available()) {
                String c = RPI_Serial.readStringUntil('\n');
                if (c == "GPS_RAW_OFF") {
                    RPI_Serial.println("{\"type\":\"mode\",\"raw\":false}");
                    break;
                }
            }
        }
    }
}

void sendGpsJson() {
    RPI_Serial.print("{\"type\":\"gps\"");

    if (gps.location.isValid()) {
        RPI_Serial.print(",\"lat\":");
        RPI_Serial.print(gps.location.lat(), 7);
        RPI_Serial.print(",\"lng\":");
        RPI_Serial.print(gps.location.lng(), 7);
    }

    if (gps.altitude.isValid()) {
        RPI_Serial.print(",\"alt\":");
        RPI_Serial.print(gps.altitude.meters(), 1);
    }

    if (gps.speed.isValid()) {
        RPI_Serial.print(",\"speed\":");
        RPI_Serial.print(gps.speed.kmph(), 2);
    }

    if (gps.course.isValid()) {
        RPI_Serial.print(",\"course\":");
        RPI_Serial.print(gps.course.deg(), 1);
    }

    RPI_Serial.print(",\"sats\":");
    RPI_Serial.print(gps.satellites.value());

    if (gps.hdop.isValid()) {
        RPI_Serial.print(",\"hdop\":");
        RPI_Serial.print(gps.hdop.hdop(), 2);
    }

    RPI_Serial.print(",\"fix\":");
    RPI_Serial.print(gpsHasFix ? "true" : "false");

    if (gps.date.isValid() && gps.time.isValid()) {
        char timebuf[32];
        snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
            gps.date.year(), gps.date.month(), gps.date.day(),
            gps.time.hour(), gps.time.minute(), gps.time.second());
        RPI_Serial.print(",\"time\":\"");
        RPI_Serial.print(timebuf);
        RPI_Serial.print("\"");
    }

    RPI_Serial.println("}");
}

void sendHeartbeat() {
    RPI_Serial.print("{\"type\":\"heartbeat\"");
    RPI_Serial.print(",\"uptime\":");
    RPI_Serial.print(millis() / 1000);
    RPI_Serial.print(",\"gps_ok\":");
    RPI_Serial.print(gpsHasFix ? "true" : "false");
    RPI_Serial.print(",\"sats\":");
    RPI_Serial.print(gps.satellites.value());
    RPI_Serial.print(",\"gps_rate\":");
    RPI_Serial.print(gpsUpdateInterval);
    RPI_Serial.print(",\"watchdog\":");
    RPI_Serial.print(rpiWatchdogEnabled ? "true" : "false");
    RPI_Serial.println("}");
}

void sendAck(const char* cmd, const char* status) {
    RPI_Serial.print("{\"type\":\"ack\",\"cmd\":\"");
    RPI_Serial.print(cmd);
    RPI_Serial.print("\",\"status\":\"");
    RPI_Serial.print(status);
    RPI_Serial.println("\"}");
}

void sendError(const char* msg) {
    RPI_Serial.print("{\"type\":\"error\",\"msg\":\"");
    RPI_Serial.print(msg);
    RPI_Serial.println("\"}");
}

void enterSleep(int seconds) {
    // Disable GPS LNA to save power
    digitalWrite(PIN_LNA_EN, LOW);

    if (seconds > 0) {
        // Timed deep sleep
        esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    }

    // Can also wake from UART activity (RXD0)
    // esp_sleep_enable_uart_wakeup(0);

    // Enter deep sleep
    esp_deep_sleep_start();
}
