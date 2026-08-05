/*
 * Raspberry Pi Backup Power and GPS Hat v4 - Unified Firmware
 *
 * Features:
 *   - GPS: Read NMEA from NEO-M9N, provide data via SPI slave
 *   - WiFi: Connect as client to upstream network
 *   - PPP: Provide network to RPi via PPP over UART
 *   - SPI Slave: Respond to RPi commands, send GPS data
 *
 * Hardware:
 *   - ESP32-S3-MINI-1 module
 *   - NEO-M9N GPS on UART1 (GPIO17/18)
 *   - RPi UART0 (GPIO43/44) for PPP
 *   - RPi SPI via GPIO4 (CS), shared bus with MCP2515 CAN controllers
 *
 * Communication:
 *   - UART0: PPP link for network sharing
 *   - SPI Slave: Commands and GPS JSON responses
 *
 * SPI Protocol:
 *   RPi sends command byte, ESP32 responds with data
 *   Commands:
 *     0x01 = Get GPS JSON
 *     0x02 = Get status JSON
 *     0x03 = Get WiFi status
 *     0x10 = Set WiFi SSID (followed by null-terminated string)
 *     0x11 = Set WiFi password (followed by null-terminated string)
 *     0x12 = Connect WiFi
 *     0x13 = Disconnect WiFi
 *     0x20 = Start PPP mode
 *     0x21 = Stop PPP mode
 *     0xFE = Ping (responds with 0xFE)
 *     0xFF = Reset ESP32
 */

#include <Arduino.h>
#include <WiFi.h>
#include <TinyGPSPlus.h>
#include <Preferences.h>
#include "driver/spi_slave.h"
#include "esp_wifi.h"
#include "ppp_server.h"

// =============================================================================
// Pin Definitions
// =============================================================================

// SPI Slave pins (directly from schematic)
#define PIN_SPI_MOSI  11   // IO11 = MOSI from RPi GPIO10
#define PIN_SPI_MISO  13   // IO13 = MISO to RPi GPIO9
#define PIN_SPI_SCK   12   // IO12 = SCK from RPi GPIO11
#define PIN_SPI_CS    10   // IO10 = CS from RPi GPIO4

// Control pins from RPi
#define PIN_HANDSHAKE 3    // IO3 = RPi GPIO5 (data ready signal TO RPi)
#define PIN_DATA_RDY  4    // IO4 = RPi GPIO6 (unused for now)

// GPS UART1 pins
#define PIN_GPS_TX    17   // To GPS RX
#define PIN_GPS_RX    18   // From GPS TX
#define PIN_LNA_EN    8    // GPS LNA enable

// RPi UART0 pins (for PPP)
#define PIN_RPI_TX    43   // To RPi RX
#define PIN_RPI_RX    44   // From RPi TX

// =============================================================================
// Constants
// =============================================================================

#define GPS_BAUD           38400
#define PPP_BAUD           115200
#define SPI_BUFFER_SIZE    256
#define GPS_UPDATE_MS      1000
#define STATUS_UPDATE_MS   5000

// SPI Commands
#define CMD_GET_GPS        0x01
#define CMD_GET_STATUS     0x02
#define CMD_GET_WIFI       0x03
#define CMD_SET_SSID       0x10
#define CMD_SET_PASS       0x11
#define CMD_WIFI_CONNECT   0x12
#define CMD_WIFI_DISCONNECT 0x13
#define CMD_PPP_START      0x20
#define CMD_PPP_STOP       0x21
#define CMD_PING           0xFE
#define CMD_RESET          0xFF

// =============================================================================
// Objects
// =============================================================================

TinyGPSPlus gps;
HardwareSerial GPS_Serial(1);   // UART1 for GPS
HardwareSerial PPP_Serial(0);   // UART0 for PPP
Preferences prefs;

// PPP Server
PPPServer pppServer(PPP_Serial);

// SPI slave buffers
WORD_ALIGNED_ATTR uint8_t spiRxBuf[SPI_BUFFER_SIZE];
WORD_ALIGNED_ATTR uint8_t spiTxBuf[SPI_BUFFER_SIZE];

// State
String wifiSSID = "";
String wifiPass = "";
bool wifiConnected = false;
bool pppActive = false;
bool gpsHasFix = false;

// Timing
unsigned long lastGpsUpdate = 0;
unsigned long lastStatusUpdate = 0;
unsigned long bootTime = 0;

// GPS JSON buffer (pre-built for fast SPI response)
char gpsJsonBuf[256] = "{\"type\":\"gps\",\"fix\":false}";
char statusJsonBuf[256] = "{\"type\":\"status\"}";
char wifiJsonBuf[256] = "{\"type\":\"wifi\",\"connected\":false}";

// =============================================================================
// Forward Declarations
// =============================================================================

void setupSpiSlave();
void setupGps();
void setupWifi();
void processGps();
void processSpiCommand();
void buildGpsJson();
void buildStatusJson();
void buildWifiJson();
void handleSpiTransaction(spi_slave_transaction_t *trans);
void connectWifi();
void disconnectWifi();
void startPpp();
void stopPpp();

// =============================================================================
// SPI Slave Callback
// =============================================================================

// Called after SPI transaction completes
void IRAM_ATTR spiTransactionComplete(spi_slave_transaction_t *trans) {
    // Signal that we're ready for next transaction
    gpio_set_level((gpio_num_t)PIN_HANDSHAKE, 0);
}

// =============================================================================
// Setup
// =============================================================================

void setup() {
    bootTime = millis();

    // FIRST: Initialize UART for debug output
    PPP_Serial.begin(PPP_BAUD, SERIAL_8N1, PIN_RPI_RX, PIN_RPI_TX);
    delay(100);  // Give UART time to initialize

    // Send early startup message
    PPP_Serial.println("{\"type\":\"boot\",\"msg\":\"ESP32 starting...\"}");

    // Initialize handshake pin (active low = ready)
    pinMode(PIN_HANDSHAKE, OUTPUT);
    digitalWrite(PIN_HANDSHAKE, HIGH);  // Not ready initially

    // Initialize GPS
    PPP_Serial.println("{\"type\":\"init\",\"step\":\"gps\"}");
    setupGps();

    // Load WiFi credentials
    PPP_Serial.println("{\"type\":\"init\",\"step\":\"prefs\"}");
    prefs.begin("wifi", true);
    wifiSSID = prefs.getString("ssid", "");
    wifiPass = prefs.getString("pass", "");
    prefs.end();

    // Initialize WiFi
    PPP_Serial.println("{\"type\":\"init\",\"step\":\"wifi\"}");
    setupWifi();

    // Initialize SPI slave (optional - may fail if no overlay)
    PPP_Serial.println("{\"type\":\"init\",\"step\":\"spi\"}");
    setupSpiSlave();

    // Send startup complete message
    PPP_Serial.println("{\"type\":\"startup\",\"device\":\"rpi-gps-hat-v4\",\"version\":\"2.0.0\",\"mode\":\"unified\"}");

    // Build initial JSON buffers
    buildGpsJson();
    buildStatusJson();
    buildWifiJson();

    // Signal ready
    digitalWrite(PIN_HANDSHAKE, LOW);
}

// =============================================================================
// Main Loop
// =============================================================================

void loop() {
    unsigned long now = millis();

    // Debug heartbeat every 5 seconds (when not in PPP mode)
    static unsigned long lastDebugMsg = 0;
    if (!pppActive && (now - lastDebugMsg >= 5000)) {
        lastDebugMsg = now;
        PPP_Serial.print("{\"type\":\"heartbeat\",\"uptime\":");
        PPP_Serial.print(now / 1000);
        PPP_Serial.print(",\"gps_fix\":");
        PPP_Serial.print(gpsHasFix ? "true" : "false");
        PPP_Serial.print(",\"wifi\":");
        PPP_Serial.print(wifiConnected ? "true" : "false");
        PPP_Serial.println("}");
    }

    // Process GPS data
    processGps();

    // Update GPS JSON periodically
    if (now - lastGpsUpdate >= GPS_UPDATE_MS) {
        lastGpsUpdate = now;
        buildGpsJson();
    }

    // Update status JSON periodically
    if (now - lastStatusUpdate >= STATUS_UPDATE_MS) {
        lastStatusUpdate = now;
        buildStatusJson();
        buildWifiJson();
    }

    // Handle WiFi events
    wifiConnected = (WiFi.status() == WL_CONNECTED);

    // Handle UART0 based on mode
    if (pppActive) {
        // Process PPP frames
        pppServer.process();

        // Update PPP state
        if (!pppServer.isConnected() && pppServer.getState() == PPP_STATE_ERROR) {
            pppActive = false;
        }
    } else if (PPP_Serial.available()) {
        // Command mode - handle text commands
        String cmd = PPP_Serial.readStringUntil('\n');
        cmd.trim();

        // Simple commands before PPP mode
        if (cmd == "STATUS") {
            PPP_Serial.println(statusJsonBuf);
        } else if (cmd == "GPS") {
            PPP_Serial.println(gpsJsonBuf);
        } else if (cmd == "WIFI") {
            PPP_Serial.println(wifiJsonBuf);
        } else if (cmd == "PPP_START") {
            PPP_Serial.println("{\"type\":\"ack\",\"cmd\":\"ppp\",\"status\":\"starting\"}");
            delay(100);
            startPpp();
        } else if (cmd == "PPP_STOP") {
            stopPpp();
            PPP_Serial.println("{\"type\":\"ack\",\"cmd\":\"ppp\",\"status\":\"stopped\"}");
        } else if (cmd == "RESET") {
            PPP_Serial.println("{\"type\":\"ack\",\"cmd\":\"reset\"}");
            delay(100);
            ESP.restart();
        } else if (cmd.startsWith("WIFI_SSID ")) {
            wifiSSID = cmd.substring(10);
            prefs.begin("wifi", false);
            prefs.putString("ssid", wifiSSID);
            prefs.end();
            PPP_Serial.println("{\"type\":\"ack\",\"cmd\":\"wifi_ssid\"}");
        } else if (cmd.startsWith("WIFI_PASS ")) {
            wifiPass = cmd.substring(10);
            prefs.begin("wifi", false);
            prefs.putString("pass", wifiPass);
            prefs.end();
            PPP_Serial.println("{\"type\":\"ack\",\"cmd\":\"wifi_pass\"}");
        } else if (cmd == "WIFI_CONNECT") {
            connectWifi();
            PPP_Serial.println("{\"type\":\"ack\",\"cmd\":\"wifi_connect\"}");
        } else if (cmd == "WIFI_DISCONNECT") {
            disconnectWifi();
            PPP_Serial.println("{\"type\":\"ack\",\"cmd\":\"wifi_disconnect\"}");
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

void buildStatusJson() {
    unsigned long uptime = (millis() - bootTime) / 1000;
    sprintf(statusJsonBuf,
        "{\"type\":\"status\",\"uptime\":%lu,\"gps_ok\":%s,\"sats\":%d,\"wifi\":%s,\"ppp\":%s}",
        uptime,
        gpsHasFix ? "true" : "false",
        gps.satellites.value(),
        wifiConnected ? "true" : "false",
        pppActive ? "true" : "false");
}

void buildWifiJson() {
    if (wifiConnected) {
        sprintf(wifiJsonBuf,
            "{\"type\":\"wifi\",\"connected\":true,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
            WiFi.SSID().c_str(),
            WiFi.localIP().toString().c_str(),
            WiFi.RSSI());
    } else {
        sprintf(wifiJsonBuf,
            "{\"type\":\"wifi\",\"connected\":false,\"ssid\":\"%s\"}",
            wifiSSID.c_str());
    }
}

// =============================================================================
// WiFi Functions
// =============================================================================

void setupWifi() {
    WiFi.mode(WIFI_STA);

    // Auto-connect if credentials exist
    if (wifiSSID.length() > 0) {
        WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    }
}

void connectWifi() {
    if (wifiSSID.length() > 0) {
        WiFi.disconnect();
        delay(100);
        WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    }
}

void disconnectWifi() {
    WiFi.disconnect();
}

// =============================================================================
// SPI Slave Functions
// =============================================================================

void setupSpiSlave() {
    // SPI slave configuration
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_BUFFER_SIZE,
    };

    spi_slave_interface_config_t slvcfg = {
        .spics_io_num = PIN_SPI_CS,
        .flags = 0,
        .queue_size = 3,
        .mode = 0,  // SPI mode 0
        .post_setup_cb = NULL,
        .post_trans_cb = spiTransactionComplete,
    };

    // Initialize SPI slave
    esp_err_t ret = spi_slave_initialize(SPI2_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        PPP_Serial.printf("{\"type\":\"error\",\"msg\":\"SPI init failed: %d\"}\n", ret);
    }
}

void processSpiCommand() {
    // Prepare transaction
    spi_slave_transaction_t trans = {
        .length = SPI_BUFFER_SIZE * 8,
        .tx_buffer = spiTxBuf,
        .rx_buffer = spiRxBuf,
    };

    // Clear buffers
    memset(spiRxBuf, 0, SPI_BUFFER_SIZE);
    memset(spiTxBuf, 0, SPI_BUFFER_SIZE);

    // Wait for transaction (non-blocking check)
    if (spi_slave_transmit(SPI2_HOST, &trans, 0) == ESP_OK) {
        // Process received command
        uint8_t cmd = spiRxBuf[0];

        switch (cmd) {
            case CMD_GET_GPS:
                memcpy(spiTxBuf, gpsJsonBuf, strlen(gpsJsonBuf) + 1);
                break;

            case CMD_GET_STATUS:
                memcpy(spiTxBuf, statusJsonBuf, strlen(statusJsonBuf) + 1);
                break;

            case CMD_GET_WIFI:
                memcpy(spiTxBuf, wifiJsonBuf, strlen(wifiJsonBuf) + 1);
                break;

            case CMD_SET_SSID:
                wifiSSID = String((char*)&spiRxBuf[1]);
                prefs.begin("wifi", false);
                prefs.putString("ssid", wifiSSID);
                prefs.end();
                strcpy((char*)spiTxBuf, "{\"ack\":\"ssid\"}");
                break;

            case CMD_SET_PASS:
                wifiPass = String((char*)&spiRxBuf[1]);
                prefs.begin("wifi", false);
                prefs.putString("pass", wifiPass);
                prefs.end();
                strcpy((char*)spiTxBuf, "{\"ack\":\"pass\"}");
                break;

            case CMD_WIFI_CONNECT:
                connectWifi();
                strcpy((char*)spiTxBuf, "{\"ack\":\"connect\"}");
                break;

            case CMD_WIFI_DISCONNECT:
                disconnectWifi();
                strcpy((char*)spiTxBuf, "{\"ack\":\"disconnect\"}");
                break;

            case CMD_PPP_START:
                startPpp();
                strcpy((char*)spiTxBuf, "{\"ack\":\"ppp_start\"}");
                break;

            case CMD_PPP_STOP:
                stopPpp();
                strcpy((char*)spiTxBuf, "{\"ack\":\"ppp_stop\"}");
                break;

            case CMD_PING:
                spiTxBuf[0] = 0xFE;
                break;

            case CMD_RESET:
                ESP.restart();
                break;

            default:
                strcpy((char*)spiTxBuf, "{\"error\":\"unknown_cmd\"}");
                break;
        }

        // Signal data ready
        digitalWrite(PIN_HANDSHAKE, LOW);
    }
}

// =============================================================================
// PPP Functions
// =============================================================================

void startPpp() {
    if (!wifiConnected) {
        PPP_Serial.println("{\"type\":\"error\",\"msg\":\"wifi_not_connected\"}");
        return;
    }

    if (!pppServer.init()) {
        PPP_Serial.println("{\"type\":\"error\",\"msg\":\"ppp_init_failed\"}");
        return;
    }

    if (!pppServer.start()) {
        PPP_Serial.println("{\"type\":\"error\",\"msg\":\"ppp_start_failed\"}");
        return;
    }

    pppActive = true;
}

void stopPpp() {
    pppServer.stop();
    pppActive = false;
}
