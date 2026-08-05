/*
 * Raspberry Pi Backup Power and GPS Hat v4 - ESP32-S3 Firmware
 *
 * Purpose:
 *   - Read NMEA strings from NEO-M9N GPS module
 *   - Relay position data to the Raspberry Pi via UART0
 *   - Keep SPI pins in high-impedance to not interfere with RPi <-> MCP2515
 *
 * Hardware:
 *   - ESP32-S3-MINI-1 module
 *   - NEO-M9N GPS module (u-blox) on UART1 (TXD1/RXD1)
 *   - RPi connected via UART0 (TXD0/RXD0 = GPIO43/44)
 *
 * IMPORTANT: The SPI bus is shared with the Raspberry Pi and MCP2515 CAN
 * controllers. This firmware keeps SPI pins as inputs to avoid interference.
 */

#include <Arduino.h>
#include <TinyGPSPlus.h>

// Pin Definitions - SPI (set as inputs - shared bus with RPi)
#define PIN_SPI_MOSI  11
#define PIN_SPI_MISO  13
#define PIN_SPI_SCK   12
#define PIN_SPI_CS    4

// Pin Definitions - GPS (UART1)
#define PIN_GPS_TX    17    // ESP32 TX -> GPS RX
#define PIN_GPS_RX    18    // ESP32 RX <- GPS TX
#define PIN_LNA_EN    8     // GPS LNA Enable (active high)

// Pin Definitions - RPi UART0
#define PIN_RPI_TX    43    // ESP32 TXD0 -> RPi RXD
#define PIN_RPI_RX    44    // ESP32 RXD0 <- RPi TXD

// GPS Serial (UART1)
// NEO-M9N default is 38400 baud
#define GPS_BAUD      38400

// RPi Serial (UART0)
#define RPI_BAUD      115200

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
HardwareSerial GPS_Serial(1);   // UART1 for GPS
HardwareSerial RPI_Serial(0);   // UART0 for RPi

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

    // Initialize UART0 for RPi communication
    RPI_Serial.begin(RPI_BAUD, SERIAL_8N1, PIN_RPI_RX, PIN_RPI_TX);

    // Initialize GPS LNA - must be HIGH to enable the antenna LNA
    pinMode(PIN_LNA_EN, OUTPUT);
    digitalWrite(PIN_LNA_EN, HIGH);

    // Initialize GPS serial (UART1)
    GPS_Serial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

    // Startup message to RPi
    RPI_Serial.println("{\"event\":\"startup\",\"device\":\"rpi-gps-hat-v4\",\"gps\":\"NEO-M9N\",\"spi\":\"high-z\"}");
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
            RPI_Serial.print(sentence);
            break;

        case OUTPUT_NMEA_FILTERED:
            // Only output position-relevant sentences
            if (strstr(sentence, "$GNGGA") || strstr(sentence, "$GPGGA") ||
                strstr(sentence, "$GNRMC") || strstr(sentence, "$GPRMC") ||
                strstr(sentence, "$GNGSA") || strstr(sentence, "$GPGSA")) {
                RPI_Serial.print(sentence);
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
    RPI_Serial.print("{\"lat\":");
    RPI_Serial.print(gps.location.lat(), 8);
    RPI_Serial.print(",\"lng\":");
    RPI_Serial.print(gps.location.lng(), 8);

    if (gps.altitude.isValid()) {
        RPI_Serial.print(",\"alt\":");
        RPI_Serial.print(gps.altitude.meters(), 2);
    }

    if (gps.speed.isValid()) {
        RPI_Serial.print(",\"speed\":");
        RPI_Serial.print(gps.speed.kmph(), 2);
    }

    if (gps.course.isValid()) {
        RPI_Serial.print(",\"course\":");
        RPI_Serial.print(gps.course.deg(), 2);
    }

    RPI_Serial.print(",\"sats\":");
    RPI_Serial.print(gps.satellites.value());

    if (gps.hdop.isValid()) {
        RPI_Serial.print(",\"hdop\":");
        RPI_Serial.print(gps.hdop.hdop(), 2);
    }

    if (gps.date.isValid() && gps.time.isValid()) {
        RPI_Serial.print(",\"time\":\"");
        char timebuf[32];
        snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02dT%02d:%02d:%02d.%02dZ",
            gps.date.year(), gps.date.month(), gps.date.day(),
            gps.time.hour(), gps.time.minute(), gps.time.second(),
            gps.time.centisecond());
        RPI_Serial.print(timebuf);
        RPI_Serial.print("\"");
    }

    RPI_Serial.println("}");
    lastFixTime = millis();
}

void processCommands() {
    if (!RPI_Serial.available()) return;

    String cmd = RPI_Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd == "RAW") {
        outputMode = OUTPUT_NMEA_RAW;
        RPI_Serial.println("{\"mode\":\"raw\"}");
    }
    else if (cmd == "FILTERED") {
        outputMode = OUTPUT_NMEA_FILTERED;
        RPI_Serial.println("{\"mode\":\"filtered\"}");
    }
    else if (cmd == "JSON") {
        outputMode = OUTPUT_JSON;
        RPI_Serial.println("{\"mode\":\"json\"}");
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
    RPI_Serial.print("{\"status\":{");
    RPI_Serial.print("\"uptime\":");
    RPI_Serial.print(millis() / 1000);
    RPI_Serial.print(",\"messages\":");
    RPI_Serial.print(messagesReceived);
    RPI_Serial.print(",\"fix\":");
    RPI_Serial.print(gps.location.isValid() ? "true" : "false");
    RPI_Serial.print(",\"sats\":");
    RPI_Serial.print(gps.satellites.value());
    RPI_Serial.print(",\"mode\":\"");
    RPI_Serial.print(outputMode == OUTPUT_NMEA_RAW ? "raw" :
                 outputMode == OUTPUT_NMEA_FILTERED ? "filtered" : "json");
    RPI_Serial.print("\",\"spi\":\"high-z\"");
    RPI_Serial.println("}}");
}
