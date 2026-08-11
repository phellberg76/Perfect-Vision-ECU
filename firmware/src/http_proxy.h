/*
 * http_proxy.h - HTTP Proxy over UART
 *
 * ESP32 acts as WiFi AP and HTTP server.
 * Forwards all HTTP requests to RPi over UART.
 * RPi handles requests via curl to localhost and returns response.
 */

#ifndef HTTP_PROXY_H
#define HTTP_PROXY_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

// Configuration
#define HTTP_PROXY_PORT       8080
#define HTTP_PROXY_TIMEOUT_MS 10000
#define HTTP_PROXY_MAX_BODY   8192

// AP Configuration
#define AP_DEFAULT_SSID       "GPS-Hat"
#define AP_DEFAULT_PASS       "gpshat123"
#define AP_CHANNEL            6
#define AP_MAX_CONNECTIONS    4

class HttpProxy {
public:
    HttpProxy(HardwareSerial& serial);

    // Initialize AP mode and HTTP server
    bool begin(const char* ssid = AP_DEFAULT_SSID,
               const char* password = AP_DEFAULT_PASS);

    // Stop AP and HTTP server
    void stop();

    // Process incoming HTTP requests (call in loop)
    void process();

    // Check if AP is active
    bool isActive() const { return _active; }

    // Get number of connected clients
    int getClientCount() const;

    // Get AP IP address
    IPAddress getIP() const;

private:
    HardwareSerial& _serial;
    WebServer* _server;
    bool _active;
    uint32_t _requestId;

    // Handle all HTTP requests
    void handleRequest();

    // Send request to RPi and wait for response
    bool forwardToRpi(const String& method, const String& path,
                      const String& headers, const String& body,
                      int& statusCode, String& responseHeaders,
                      String& responseBody);

    // Parse response from RPi
    bool parseResponse(const String& response, int& statusCode,
                       String& headers, String& body);

    // Read line from serial with timeout
    String readLineWithTimeout(unsigned long timeoutMs);
};

#endif // HTTP_PROXY_H
