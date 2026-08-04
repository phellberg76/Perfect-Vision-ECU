/*
 * Raspberry Pi Backup Power and GPS Hat v4 - ESP32-S3 Firmware
 *
 * Purpose:
 *   - Read NMEA strings from NEO-M9N GPS module
 *   - Relay position data to the Raspberry Pi
 *   - Keep SPI pins in high-impedance to not interfere with RPi <-> MCP2515
 *
 * Hardware:
 *   - ESP32-S3-MINI-1 module
 *   - NEO-M9N GPS module (u-blox)
 *   - USB-C connection to Raspberry Pi
 *
 * IMPORTANT: The SPI bus is shared with the Raspberry Pi and MCP2515 CAN
 * controllers. This firmware keeps SPI pins as inputs to avoid interference.
 */

#include <Arduino.h>
#include <TinyGPSPlus.h>

// Pin Definitions - SPI (directly expose as inputs - shared bus with RPi)
#define PIN_SPI_MOSI  11
#define PIN_SPI_MISO  13
#define PIN_SPI_SCK   12
#define PIN_SPI_CS    4   // GPIO4_CS from schematic

// Pin Definitions - GPS (UART1)
#define PIN_GPS_TX    17    // ESP32 TX -> GPS RX
#define PIN_GPS_RX    18    // ESP32 RX <- GPS TX
#define PIN_LNA_EN    8     // GPS LNA Enable

// Pin Definitions - Status
#define PIN_LED       4

// GPS Serial
#define GPS_BAUD      9600

// Output modes
enum OutputMode {
    OUTPUT_NMEA_RAW,        // Pass through raw NMEA sentences
    OUTPUT_NMEA_FILTERED,   // Only GGA, RMC, GSA sentences
    OUTPUT_JSON             // Parsed JSON format
};

// Configuration
OutputMode outputMode = OUTPUT_NMEA_FILTERED;

// Objects
TinyGPSPlus gps;
HardwareSerial GPS_Serial(1);

// Buffers
char nmeaBuffer[256];
int nmeaIndex = 0;

// Statistics
unsigned long messagesReceived = 0;
unsigned long lastFixTime = 0;

// Forward declarations
void processNMEA(const char* sentence);
void processCommands();
void outputJSON();
void printStatus();

void setup() {
    // CRITICAL: Set SPI pins as inputs immediately to avoid bus contention
    // The SPI bus is shared with RPi <-> MCP2515 CAN controllers
    pinMode(PIN_SPI_MOSI, INPUT);
    pinMode(PIN_SPI_MISO, INPUT);
    pinMode(PIN_SPI_SCK, INPUT);
    pinMode(PIN_SPI_CS, INPUT);

    // Initialize USB serial for RPi communication
    Serial.begin(115200);
    while (!Serial && millis() < 3000);

    // Initialize LED (use different pin if PIN_LED conflicts with SPI_CS)
    // pinMode(PIN_LED, OUTPUT);
    // digitalWrite(PIN_LED, LOW);

    // Initialize GPS LNA
    pinMode(PIN_LNA_EN, OUTPUT);
    digitalWrite(PIN_LNA_EN, HIGH);

    // Initialize GPS serial
    GPS_Serial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

    // Startup message
    Serial.println("{\"event\":\"startup\",\"device\":\"rpi-gps-hat-v4\",\"gps\":\"NEO-M9N\",\"spi\":\"high-z\"}");
}

void loop() {
    // Process incoming GPS data
    while (GPS_Serial.available()) {
        char c = GPS_Serial.read();

        // Feed to TinyGPS++ for parsing
        gps.encode(c);

        // Buffer NMEA sentences
        if (c == '$') {
            nmeaIndex = 0;
        }

        if (nmeaIndex < sizeof(nmeaBuffer) - 1) {
            nmeaBuffer[nmeaIndex++] = c;
        }

        if (c == '\n') {
            nmeaBuffer[nmeaIndex] = '\0';
            processNMEA(nmeaBuffer);
            nmeaIndex = 0;
        }
    }

    // Process commands from RPi
    processCommands();
}

void processNMEA(const char* sentence) {
    messagesReceived++;

    switch (outputMode) {
        case OUTPUT_NMEA_RAW:
            Serial.print(sentence);
            break;

        case OUTPUT_NMEA_FILTERED:
            // Only output position-relevant sentences
            if (strstr(sentence, "$GNGGA") || strstr(sentence, "$GPGGA") ||
                strstr(sentence, "$GNRMC") || strstr(sentence, "$GPRMC") ||
                strstr(sentence, "$GNGSA") || strstr(sentence, "$GPGSA")) {
                Serial.print(sentence);
            }
            break;

        case OUTPUT_JSON:
            // Output parsed data when we have a valid location update
            if (gps.location.isUpdated() && gps.location.isValid()) {
                outputJSON();
            }
            break;
    }
}

void outputJSON() {
    Serial.print("{\"lat\":");
    Serial.print(gps.location.lat(), 8);
    Serial.print(",\"lng\":");
    Serial.print(gps.location.lng(), 8);

    if (gps.altitude.isValid()) {
        Serial.print(",\"alt\":");
        Serial.print(gps.altitude.meters(), 2);
    }

    if (gps.speed.isValid()) {
        Serial.print(",\"speed\":");
        Serial.print(gps.speed.kmph(), 2);
    }

    if (gps.course.isValid()) {
        Serial.print(",\"course\":");
        Serial.print(gps.course.deg(), 2);
    }

    Serial.print(",\"sats\":");
    Serial.print(gps.satellites.value());

    if (gps.hdop.isValid()) {
        Serial.print(",\"hdop\":");
        Serial.print(gps.hdop.hdop(), 2);
    }

    if (gps.date.isValid() && gps.time.isValid()) {
        Serial.print(",\"time\":\"");
        Serial.printf("%04d-%02d-%02dT%02d:%02d:%02d.%02dZ",
            gps.date.year(), gps.date.month(), gps.date.day(),
            gps.time.hour(), gps.time.minute(), gps.time.second(),
            gps.time.centisecond());
        Serial.print("\"");
    }

    Serial.println("}");
    lastFixTime = millis();
}

void processCommands() {
    if (!Serial.available()) return;

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd == "RAW") {
        outputMode = OUTPUT_NMEA_RAW;
        Serial.println("{\"mode\":\"raw\"}");
    }
    else if (cmd == "FILTERED") {
        outputMode = OUTPUT_NMEA_FILTERED;
        Serial.println("{\"mode\":\"filtered\"}");
    }
    else if (cmd == "JSON") {
        outputMode = OUTPUT_JSON;
        Serial.println("{\"mode\":\"json\"}");
    }
    else if (cmd == "STATUS") {
        printStatus();
    }
    else if (cmd == "RESET") {
        ESP.restart();
    }
    else if (cmd.startsWith("GPS:")) {
        // Pass through commands to GPS module
        String gpsCmd = cmd.substring(4);
        GPS_Serial.println(gpsCmd);
    }
}

void printStatus() {
    Serial.print("{\"status\":{");
    Serial.print("\"uptime\":");
    Serial.print(millis() / 1000);
    Serial.print(",\"messages\":");
    Serial.print(messagesReceived);
    Serial.print(",\"fix\":");
    Serial.print(gps.location.isValid() ? "true" : "false");
    Serial.print(",\"sats\":");
    Serial.print(gps.satellites.value());
    Serial.print(",\"mode\":\"");
    Serial.print(outputMode == OUTPUT_NMEA_RAW ? "raw" :
                 outputMode == OUTPUT_NMEA_FILTERED ? "filtered" : "json");
    Serial.print("\",\"spi\":\"high-z\"");
    Serial.println("}}");
}
