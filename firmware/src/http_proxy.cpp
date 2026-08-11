/*
 * http_proxy.cpp - HTTP Proxy over UART
 */

#include "http_proxy.h"
#include <ArduinoJson.h>

HttpProxy::HttpProxy(HardwareSerial& serial)
    : _serial(serial), _server(nullptr), _active(false), _requestId(0) {
}

bool HttpProxy::begin(const char* ssid, const char* password) {
    Serial.println("[HTTP_PROXY] Starting AP mode...");

    // Configure AP
    WiFi.mode(WIFI_AP_STA);  // AP + Station mode

    if (!WiFi.softAP(ssid, password, AP_CHANNEL, false, AP_MAX_CONNECTIONS)) {
        Serial.println("[HTTP_PROXY] Failed to start AP");
        return false;
    }

    // Wait for AP to start
    delay(100);

    Serial.printf("[HTTP_PROXY] AP started: %s\n", ssid);
    Serial.printf("[HTTP_PROXY] AP IP: %s\n", WiFi.softAPIP().toString().c_str());

    // Create and configure HTTP server
    _server = new WebServer(HTTP_PROXY_PORT);

    // Catch-all handler for any request
    _server->onNotFound([this]() {
        this->handleRequest();
    });

    _server->begin();
    _active = true;

    Serial.printf("[HTTP_PROXY] HTTP server started on port %d\n", HTTP_PROXY_PORT);

    // Notify RPi
    _serial.println("{\"type\":\"http_proxy\",\"status\":\"started\",\"ssid\":\"" +
                    String(ssid) + "\",\"ip\":\"" + WiFi.softAPIP().toString() + "\"}");

    return true;
}

void HttpProxy::stop() {
    if (_server) {
        _server->stop();
        delete _server;
        _server = nullptr;
    }

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);

    _active = false;

    Serial.println("[HTTP_PROXY] Stopped");
    _serial.println("{\"type\":\"http_proxy\",\"status\":\"stopped\"}");
}

void HttpProxy::process() {
    if (_server && _active) {
        _server->handleClient();
    }
}

int HttpProxy::getClientCount() const {
    return WiFi.softAPgetStationNum();
}

IPAddress HttpProxy::getIP() const {
    return WiFi.softAPIP();
}

void HttpProxy::handleRequest() {
    if (!_server) return;

    String method = "";
    switch (_server->method()) {
        case HTTP_GET:     method = "GET"; break;
        case HTTP_POST:    method = "POST"; break;
        case HTTP_PUT:     method = "PUT"; break;
        case HTTP_DELETE:  method = "DELETE"; break;
        case HTTP_PATCH:   method = "PATCH"; break;
        case HTTP_OPTIONS: method = "OPTIONS"; break;
        default:           method = "GET"; break;
    }

    String path = _server->uri();

    // Collect headers
    JsonDocument headersDoc;
    for (int i = 0; i < _server->headers(); i++) {
        headersDoc[_server->headerName(i)] = _server->header(i);
    }
    String headers;
    serializeJson(headersDoc, headers);

    // Handle body and query params
    String body = "";
    if (_server->hasArg("plain")) {
        // Raw body (JSON, etc.)
        body = _server->arg("plain");
        // For GET, args go in query string
        if (method == "GET" && _server->args() > 0) {
            path += "?";
            for (int i = 0; i < _server->args(); i++) {
                if (_server->argName(i) == "plain") continue;
                if (path.endsWith("?") == false) path += "&";
                path += _server->argName(i) + "=" + _server->arg(i);
            }
        }
    } else if (_server->args() > 0) {
        // Form data - for POST/PUT/PATCH, encode as body; for GET, as query string
        if (method == "POST" || method == "PUT" || method == "PATCH") {
            // Encode form data as urlencoded body
            for (int i = 0; i < _server->args(); i++) {
                if (i > 0) body += "&";
                body += _server->argName(i) + "=" + _server->arg(i);
            }
        } else {
            // GET - add to query string
            path += "?";
            for (int i = 0; i < _server->args(); i++) {
                if (i > 0) path += "&";
                path += _server->argName(i) + "=" + _server->arg(i);
            }
        }
    }

    Serial.printf("[HTTP_PROXY] %s %s\n", method.c_str(), path.c_str());

    // Forward to RPi
    int statusCode = 502;
    String responseHeaders = "";
    String responseBody = "{\"error\":\"gateway_timeout\"}";

    if (forwardToRpi(method, path, headers, body, statusCode, responseHeaders, responseBody)) {
        // Parse response headers and set them
        JsonDocument respHeadersDoc;
        if (deserializeJson(respHeadersDoc, responseHeaders) == DeserializationError::Ok) {
            for (JsonPair kv : respHeadersDoc.as<JsonObject>()) {
                _server->sendHeader(kv.key().c_str(), kv.value().as<String>());
            }
        }

        // Determine content type
        String contentType = "application/json";
        if (respHeadersDoc.containsKey("Content-Type")) {
            contentType = respHeadersDoc["Content-Type"].as<String>();
        } else if (respHeadersDoc.containsKey("content-type")) {
            contentType = respHeadersDoc["content-type"].as<String>();
        }

        _server->send(statusCode, contentType, responseBody);
    } else {
        _server->send(502, "application/json", "{\"error\":\"rpi_timeout\"}");
    }
}

bool HttpProxy::forwardToRpi(const String& method, const String& path,
                              const String& headers, const String& body,
                              int& statusCode, String& responseHeaders,
                              String& responseBody) {
    _requestId++;

    // Build request JSON
    JsonDocument reqDoc;
    reqDoc["type"] = "http_request";
    reqDoc["id"] = _requestId;
    reqDoc["method"] = method;
    reqDoc["path"] = path;
    reqDoc["body"] = body;

    // Parse and include headers
    JsonDocument headersDoc;
    deserializeJson(headersDoc, headers);
    reqDoc["headers"] = headersDoc;

    // Send to RPi
    String requestJson;
    serializeJson(reqDoc, requestJson);
    _serial.println(requestJson);

    Serial.printf("[HTTP_PROXY] Sent request id=%d\n", _requestId);

    // Wait for response
    unsigned long startTime = millis();
    while (millis() - startTime < HTTP_PROXY_TIMEOUT_MS) {
        if (_serial.available()) {
            String line = readLineWithTimeout(1000);
            if (line.length() == 0) continue;

            // Parse response
            JsonDocument respDoc;
            DeserializationError err = deserializeJson(respDoc, line);
            if (err) {
                Serial.printf("[HTTP_PROXY] JSON parse error: %s\n", err.c_str());
                continue;
            }

            // Check if this is our response
            if (respDoc["type"] == "http_response" && respDoc["id"] == _requestId) {
                statusCode = respDoc["status"] | 500;

                // Get headers as JSON string
                if (respDoc.containsKey("headers")) {
                    serializeJson(respDoc["headers"], responseHeaders);
                } else {
                    responseHeaders = "{}";
                }

                // Get body
                responseBody = respDoc["body"].as<String>();

                Serial.printf("[HTTP_PROXY] Got response id=%d status=%d\n",
                             _requestId, statusCode);
                return true;
            }
        }
        delay(1);
    }

    Serial.println("[HTTP_PROXY] Timeout waiting for RPi response");
    return false;
}

String HttpProxy::readLineWithTimeout(unsigned long timeoutMs) {
    String line = "";
    unsigned long start = millis();

    while (millis() - start < timeoutMs) {
        if (_serial.available()) {
            char c = _serial.read();
            if (c == '\n') {
                return line;
            } else if (c != '\r') {
                line += c;
            }
        }
    }

    return line;
}
