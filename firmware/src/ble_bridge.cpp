/*
 * ble_bridge.cpp - BLE GATT Server Implementation
 */

#include "ble_bridge.h"

// =============================================================================
// BleBridge Implementation
// =============================================================================

BleBridge::BleBridge(HardwareSerial& serial)
    : _serial(serial)
    , _pServer(nullptr)
    , _pRequestChar(nullptr)
    , _pResponseChar(nullptr)
    , _deviceConnected(false)
{
}

bool BleBridge::begin(const char* deviceName) {
    Serial.println("[BLE] Initializing NimBLE...");

    // Initialize NimBLE
    NimBLEDevice::init(deviceName);

    // Set power level for good range
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // Create server
    _pServer = NimBLEDevice::createServer();
    _pServer->setCallbacks(new BleServerCallbacks(this));

    // Setup GATT service
    setupService();

    // Start advertising
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(PLANTER_SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);  // For iOS compatibility
    pAdvertising->setMaxPreferred(0x12);
    pAdvertising->start();

    Serial.printf("[BLE] Advertising as '%s'\n", deviceName);
    Serial.printf("[BLE] Service UUID: %s\n", PLANTER_SERVICE_UUID);

    return true;
}

void BleBridge::setupService() {
    // Create service
    NimBLEService* pService = _pServer->createService(PLANTER_SERVICE_UUID);

    // Create request characteristic (app writes here)
    _pRequestChar = pService->createCharacteristic(
        REQUEST_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    _pRequestChar->setCallbacks(new RequestCharCallbacks(this));

    // Create response characteristic (bridge notifies here)
    _pResponseChar = pService->createCharacteristic(
        RESPONSE_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Start service
    pService->start();

    Serial.println("[BLE] GATT service started");
}

void BleBridge::processUart() {
    // Read incoming data from RPi
    while (_serial.available()) {
        char c = _serial.read();
        _rxBuffer += c;

        // Check for complete message (newline-delimited JSON)
        if (c == '\n') {
            String message = _rxBuffer;
            message.trim();
            _rxBuffer = "";

            if (message.length() > 0) {
                // Forward to BLE client
                sendToClient(message);
            }
        }
    }

    // Prevent buffer overflow
    if (_rxBuffer.length() > 4096) {
        Serial.println("[BLE] UART buffer overflow, clearing");
        _rxBuffer = "";
    }
}

void BleBridge::process() {
    // Process UART data from RPi
    processUart();

    // NimBLE handles events automatically via callbacks
}

bool BleBridge::isConnected() {
    return _pServer != nullptr && _pServer->getConnectedCount() > 0;
}

int BleBridge::getConnectionCount() {
    return _pServer != nullptr ? _pServer->getConnectedCount() : 0;
}

void BleBridge::sendToClient(const String& message) {
    if (!isConnected() || _pResponseChar == nullptr) {
        return;
    }

    // Add newline if not present (for message framing)
    String toSend = message;
    if (!toSend.endsWith("\n")) {
        toSend += "\n";
    }

    // Send via notifications
    // NimBLE handles chunking automatically up to MTU
    _pResponseChar->setValue((uint8_t*)toSend.c_str(), toSend.length());
    _pResponseChar->notify();

    Serial.printf("[BLE] Sent to client: %d bytes\n", toSend.length());
}

void BleBridge::onBleDataReceived(const String& data) {
    Serial.printf("[BLE] Received from client: %s\n", data.c_str());

    // Forward to RPi via UART
    _serial.println(data);
}

void BleBridge::onClientConnect() {
    _deviceConnected = true;
    Serial.println("[BLE] Client connected");
}

void BleBridge::onClientDisconnect() {
    _deviceConnected = false;
    Serial.println("[BLE] Client disconnected");

    // Restart advertising
    NimBLEDevice::getAdvertising()->start();
    Serial.println("[BLE] Advertising restarted");
}

// =============================================================================
// Server Callbacks
// =============================================================================

void BleServerCallbacks::onConnect(NimBLEServer* pServer) {
    if (_bridge) {
        _bridge->onClientConnect();
    }
}

void BleServerCallbacks::onDisconnect(NimBLEServer* pServer) {
    if (_bridge) {
        _bridge->onClientDisconnect();
    }
}

// =============================================================================
// Request Characteristic Callbacks
// =============================================================================

void RequestCharCallbacks::onWrite(NimBLECharacteristic* pCharacteristic) {
    std::string value = pCharacteristic->getValue();

    if (value.length() > 0 && _bridge != nullptr) {
        String data = String(value.c_str());
        data.trim();

        if (data.length() > 0) {
            _bridge->onBleDataReceived(data);
        }
    }
}
