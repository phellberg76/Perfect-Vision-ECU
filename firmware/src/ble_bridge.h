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

// Service and Characteristic UUIDs (must match Flutter app)
#define PLANTER_SERVICE_UUID        "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
#define REQUEST_CHAR_UUID           "a1b2c3d4-e5f6-7890-abcd-ef1234567891"
#define RESPONSE_CHAR_UUID          "a1b2c3d4-e5f6-7890-abcd-ef1234567892"

class BleBridge {
public:
    BleBridge(HardwareSerial& serial);

    // Initialize BLE with device name
    bool begin(const char* deviceName = "PerfectPrecisionPlanter");

    // Process incoming data from UART (responses from RPi)
    void processUart();

    // Process BLE events
    void process();

    // Check if a client is connected
    bool isConnected();

    // Get connection count
    int getConnectionCount();

    // Send a message to connected client (for responses from RPi)
    void sendToClient(const String& message);

    // Callback for when data is received from BLE client
    void onBleDataReceived(const String& data);

    // Connection state callbacks
    void onClientConnect();
    void onClientDisconnect();

private:
    HardwareSerial& _serial;
    NimBLEServer* _pServer;
    NimBLECharacteristic* _pRequestChar;
    NimBLECharacteristic* _pResponseChar;
    bool _deviceConnected;
    String _rxBuffer;  // Buffer for incoming UART data

    void setupService();
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
