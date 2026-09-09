/*
 * ble_bridge.h - BLE GATT Server for Planter App
 *
 * Provides BLE connectivity for the Flutter app to communicate
 * with the Raspberry Pi backend via UART.
 *
 * Service UUIDs match the original Rust BLE bridge for app compatibility.
 */

#ifndef BLE_BRIDGE_H
#define BLE_BRIDGE_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

// Service and Characteristic UUIDs (must match Flutter app)
#define PLANTER_SERVICE_UUID        "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
#define REQUEST_CHAR_UUID           "a1b2c3d4-e5f6-7890-abcd-ef1234567891"
#define RESPONSE_CHAR_UUID          "a1b2c3d4-e5f6-7890-abcd-ef1234567892"

// Message queue settings
#define BLE_MSG_QUEUE_SIZE          32      // Max messages in queue
#define BLE_MSG_MAX_SIZE            16384   // Max message size (16KB)

class BleBridge {
public:
    BleBridge(HardwareSerial& serial);

    // Initialize BLE with device name
    bool begin(const char* deviceName = "PerfectPrecisionPlanter");

    // Process incoming data from UART (responses from RPi)
    void processUart();

    // Process BLE events and message queue
    void process();

    // Check if a client is connected
    bool isConnected();

    // Get connection count
    int getConnectionCount();

    // Queue a message to send to connected client
    // Messages are sent sequentially to prevent interleaving
    void sendToClient(const String& message);

    // Callback for when data is received from BLE client
    void onBleDataReceived(const String& data);

    // Connection state callbacks
    void onClientConnect();
    void onClientDisconnect();

    // Admin command flags (set by BLE, checked/cleared by main loop)
    bool isRebootRequested() { return _adminRebootRequested; }
    void clearRebootRequest() { _adminRebootRequested = false; }
    bool isStatusRequested() { return _adminStatusRequested; }
    void clearStatusRequest() { _adminStatusRequested = false; }

    // Queue stats
    int getQueuedMessageCount();

private:
    HardwareSerial& _serial;
    NimBLEServer* _pServer;
    NimBLECharacteristic* _pRequestChar;
    NimBLECharacteristic* _pResponseChar;
    bool _deviceConnected;
    String _rxBuffer;  // Buffer for incoming UART data
    SemaphoreHandle_t _sendMutex;  // Mutex to prevent interleaved BLE sends
    QueueHandle_t _msgQueue;  // Queue for outgoing messages
    bool _isSending;  // Flag to track if currently sending
    unsigned long _lastSendComplete;  // Time last send completed

    // Admin command flags
    volatile bool _adminRebootRequested;
    volatile bool _adminStatusRequested;

    void setupService();
    void processQueue();  // Process one message from the queue
    void sendMessageDirect(const String& message);  // Actually send via BLE
};

// Server callbacks
class BleServerCallbacks : public NimBLEServerCallbacks {
public:
    BleServerCallbacks(BleBridge* bridge) : _bridge(bridge) {}

    void onConnect(NimBLEServer* pServer) override;
    void onDisconnect(NimBLEServer* pServer) override;

private:
    BleBridge* _bridge;
};

// Request characteristic callbacks (receives data from app)
class RequestCharCallbacks : public NimBLECharacteristicCallbacks {
public:
    RequestCharCallbacks(BleBridge* bridge) : _bridge(bridge) {}

    void onWrite(NimBLECharacteristic* pCharacteristic) override;

private:
    BleBridge* _bridge;
};

#endif // BLE_BRIDGE_H
