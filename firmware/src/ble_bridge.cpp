/*
 * ble_bridge.cpp - BLE GATT Server Implementation
 */

#include "ble_bridge.h"

// Minimum time between completing one message and starting the next (ms)
// This ensures the BLE stack has time to flush all notifications
#define BLE_MSG_INTERVAL_MS  50

// =============================================================================
// BleBridge Implementation
// =============================================================================

BleBridge::BleBridge(HardwareSerial& serial)
    : _serial(serial)
    , _pServer(nullptr)
    , _pRequestChar(nullptr)
    , _pResponseChar(nullptr)
    , _deviceConnected(false)
    , _sendMutex(xSemaphoreCreateMutex())
    , _msgQueue(nullptr)
    , _isSending(false)
    , _lastSendComplete(0)
    , _adminRebootRequested(false)
    , _adminStatusRequested(false)
{
    // Create message queue - stores pointers to dynamically allocated strings
    _msgQueue = xQueueCreate(BLE_MSG_QUEUE_SIZE, sizeof(char*));
    if (_msgQueue == nullptr) {
        Serial.println("[BLE] ERROR: Failed to create message queue!");
    }
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
    Serial.printf("[BLE] Message queue size: %d\n", BLE_MSG_QUEUE_SIZE);

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
    // NOTE: UART reading is now handled by processUartCommands() in main.cpp
    // to avoid two code paths reading from the same serial port.
    // Messages are forwarded to BLE via sendToClient() from handleUartCommand().
}

void BleBridge::process() {
    // Process UART data from RPi
    processUart();

    // Process outgoing message queue
    processQueue();

    // NimBLE handles events automatically via callbacks
}

void BleBridge::processQueue() {
    if (!isConnected() || _pResponseChar == nullptr || _msgQueue == nullptr) {
        // If not connected, drain the queue to prevent buildup
        if (_msgQueue != nullptr) {
            char* msg;
            while (xQueueReceive(_msgQueue, &msg, 0) == pdTRUE) {
                free(msg);
            }
        }
        return;
    }

    // Don't start a new message if we're still in the cooldown period
    unsigned long now = millis();
    if (now - _lastSendComplete < BLE_MSG_INTERVAL_MS) {
        return;
    }

    // Try to get a message from the queue (non-blocking)
    char* msg;
    if (xQueueReceive(_msgQueue, &msg, 0) == pdTRUE) {
        // Send this message
        String message(msg);
        free(msg);  // Free the allocated memory

        sendMessageDirect(message);
        _lastSendComplete = millis();
    }
}

bool BleBridge::isConnected() {
    return _pServer != nullptr && _pServer->getConnectedCount() > 0;
}

int BleBridge::getConnectionCount() {
    return _pServer != nullptr ? _pServer->getConnectedCount() : 0;
}

int BleBridge::getQueuedMessageCount() {
    if (_msgQueue == nullptr) return 0;
    return uxQueueMessagesWaiting(_msgQueue);
}

void BleBridge::sendToClient(const String& message) {
    if (!isConnected() || _pResponseChar == nullptr || _msgQueue == nullptr) {
        return;
    }

    // Check message size
    if (message.length() > BLE_MSG_MAX_SIZE) {
        Serial.printf("[BLE] Message too large (%d bytes), dropping\n", message.length());
        return;
    }

    // Allocate memory for the message and copy it
    char* msgCopy = (char*)malloc(message.length() + 1);
    if (msgCopy == nullptr) {
        Serial.println("[BLE] Failed to allocate memory for message, dropping");
        return;
    }
    strcpy(msgCopy, message.c_str());

    // Add to queue (non-blocking)
    if (xQueueSend(_msgQueue, &msgCopy, 0) != pdTRUE) {
        // Queue is full, drop the message
        Serial.println("[BLE] Message queue full, dropping message");
        free(msgCopy);
        return;
    }

    // Log queue status periodically
    int queueCount = uxQueueMessagesWaiting(_msgQueue);
    if (queueCount > 5) {
        Serial.printf("[BLE] Queued message, %d messages pending\n", queueCount);
    }
}

void BleBridge::sendMessageDirect(const String& message) {
    // Lock mutex to prevent any concurrent access
    if (xSemaphoreTake(_sendMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        Serial.println("[BLE] Failed to acquire send mutex, dropping message");
        return;
    }

    // Add newline if not present (for message framing)
    String toSend = message;
    if (!toSend.endsWith("\n")) {
        toSend += "\n";
    }

    // Use a safe chunk size (default BLE MTU is 23, minus 3 for ATT header = 20)
    // Most modern devices negotiate higher MTU, but we'll use a conservative value
    uint16_t mtu = 200;  // Safe chunk size that works with most devices

    // Send in chunks if message is larger than MTU
    size_t totalLen = toSend.length();
    size_t offset = 0;
    int chunkCount = 0;

    while (offset < totalLen) {
        size_t chunkLen = min((size_t)mtu, totalLen - offset);

        _pResponseChar->setValue((uint8_t*)(toSend.c_str() + offset), chunkLen);
        _pResponseChar->notify();

        offset += chunkLen;
        chunkCount++;

        // Delay between chunks to let BLE stack transmit
        delay(15);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Longer delay after message to ensure BLE stack flushes all notifications
    delay(30);
    vTaskDelay(pdMS_TO_TICKS(30));

    Serial.printf("[BLE] Sent: %d bytes in %d chunks\n", totalLen, chunkCount);

    xSemaphoreGive(_sendMutex);
}

void BleBridge::onBleDataReceived(const String& data) {
    Serial.printf("[BLE] Received from client: %s\n", data.c_str());

    // Check for ESP32 admin commands (not forwarded to RPi)
    if (data.indexOf("\"cmd\":\"reboot_rpi\"") >= 0) {
        Serial.println("[BLE] Admin command: reboot_rpi");
        sendToClient("{\"type\":\"admin_ack\",\"cmd\":\"reboot_rpi\",\"status\":\"rebooting\"}");
        _adminRebootRequested = true;
        return;
    }

    if (data.indexOf("\"cmd\":\"esp32_status\"") >= 0) {
        Serial.println("[BLE] Admin command: esp32_status");
        // Status will be sent by main loop
        _adminStatusRequested = true;
        return;
    }

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

    // Clear message queue on disconnect
    if (_msgQueue != nullptr) {
        char* msg;
        int cleared = 0;
        while (xQueueReceive(_msgQueue, &msg, 0) == pdTRUE) {
            free(msg);
            cleared++;
        }
        if (cleared > 0) {
            Serial.printf("[BLE] Cleared %d queued messages on disconnect\n", cleared);
        }
    }

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
