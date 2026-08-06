/*
 * SLIP (Serial Line IP) Server
 *
 * Much simpler than PPP - just byte-stuffing for IP packets.
 * Linux has built-in support via slattach.
 *
 * SLIP Framing:
 *   END (0xC0) = Frame delimiter
 *   ESC (0xDB) = Escape character
 *   ESC_END (0xDC) = Escaped END byte
 *   ESC_ESC (0xDD) = Escaped ESC byte
 *
 * Usage:
 *   1. ESP32: slipServer.start()
 *   2. RPi: slattach -p slip -s 115200 /dev/ttyAMA0
 *   3. RPi: ifconfig sl0 10.0.0.2 pointopoint 10.0.0.1 up
 *   4. RPi: route add default gw 10.0.0.1
 */

#ifndef SLIP_SERVER_H
#define SLIP_SERVER_H

#include <Arduino.h>

// SLIP special characters
#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

// Buffer size
#define SLIP_MTU     1500
#define SLIP_BUF_SIZE (SLIP_MTU + 100)

enum SlipState {
    SLIP_STATE_IDLE,
    SLIP_STATE_ACTIVE,
    SLIP_STATE_ERROR
};

class SLIPServer {
public:
    SLIPServer(HardwareSerial& serial);
    ~SLIPServer();

    bool start();
    void stop();
    void process();  // Call from main loop

    bool isActive() const { return _state == SLIP_STATE_ACTIVE; }
    SlipState getState() const { return _state; }

    uint32_t getBytesSent() const { return _bytesSent; }
    uint32_t getBytesRecv() const { return _bytesRecv; }
    uint32_t getPacketsSent() const { return _packetsSent; }
    uint32_t getPacketsRecv() const { return _packetsRecv; }

private:
    HardwareSerial& _serial;
    SlipState _state;

    uint8_t _rxBuffer[SLIP_BUF_SIZE];
    size_t _rxLen;
    bool _escaping;

    uint32_t _bytesSent;
    uint32_t _bytesRecv;
    uint32_t _packetsSent;
    uint32_t _packetsRecv;

    void processReceivedPacket(uint8_t* data, size_t len);
    void sendPacket(uint8_t* data, size_t len);
    void forwardToWifi(uint8_t* ipPacket, size_t len);
};

#endif // SLIP_SERVER_H
