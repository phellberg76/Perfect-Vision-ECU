/*
 * PPP Server for ESP32 - Network sharing to RPi
 *
 * Provides internet connectivity from ESP32 WiFi to RPi via PPP over UART.
 *
 * The ESP32 acts as a PPP server/host:
 * - Assigns IP 10.0.0.2 to RPi (peer)
 * - Uses 10.0.0.1 as local address
 * - NATs traffic from PPP to WiFi
 */

#ifndef PPP_SERVER_H
#define PPP_SERVER_H

#include <Arduino.h>
#include "esp_netif.h"
#include "lwip/ip_addr.h"

// PPP configuration
#define PPP_LOCAL_IP      "10.0.0.1"    // ESP32's PPP interface IP
#define PPP_PEER_IP       "10.0.0.2"    // RPi's PPP interface IP
#define PPP_NETMASK       "255.255.255.0"

// PPP state
typedef enum {
    PPP_STATE_IDLE,
    PPP_STATE_STARTING,
    PPP_STATE_LCP_OPEN,
    PPP_STATE_CONNECTED,
    PPP_STATE_ERROR
} ppp_state_t;

class PPPServer {
public:
    PPPServer(HardwareSerial& serial);
    ~PPPServer();

    // Initialize PPP (call once)
    bool init();

    // Start PPP negotiation
    bool start();

    // Stop PPP session
    void stop();

    // Get current state
    ppp_state_t getState() const { return _state; }

    // Check if connected
    bool isConnected() const { return _state == PPP_STATE_CONNECTED; }

    // Process PPP (call in loop when active)
    void process();

    // Get statistics
    uint32_t getBytesSent() const { return _bytesSent; }
    uint32_t getBytesReceived() const { return _bytesRecv; }

private:
    HardwareSerial& _serial;
    ppp_state_t _state;
    esp_netif_t* _pppNetif;
    uint32_t _bytesSent;
    uint32_t _bytesRecv;

    // LCP/IPCP negotiation state
    uint8_t _rxBuffer[1500];
    size_t _rxLen;
    bool _escapeNext;

    // Frame handling
    void sendPppFrame(uint8_t* data, size_t len);
    bool receivePppFrame(uint8_t* data, size_t* len);
    void processFrame(uint8_t* data, size_t len);

    // PPP protocol handlers
    void handleLcp(uint8_t* data, size_t len);
    void handleIpcp(uint8_t* data, size_t len);
    void handleIp(uint8_t* data, size_t len);

    // Send protocol messages
    void sendLcpConfigReq();
    void sendLcpConfigAck(uint8_t id, uint8_t* options, size_t optLen);
    void sendIpcpConfigReq();
    void sendIpcpConfigAck(uint8_t id, uint8_t* options, size_t optLen);
    void sendProtocolReject(uint16_t protocol, uint8_t* data, size_t len);
};

#endif // PPP_SERVER_H
