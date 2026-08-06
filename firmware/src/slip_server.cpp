/*
 * SLIP (Serial Line IP) Server Implementation
 *
 * Forwards IP packets between UART and WiFi using SLIP framing.
 * Uses lwIP for actual packet routing with NAPT.
 */

#include "slip_server.h"
#include "esp_netif.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/ip.h"
#include "lwip/ip4.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/raw.h"
#include <WiFi.h>

// For NAPT (Network Address Port Translation)
extern "C" {
    // NAPT functions if available
    #if IP_NAPT
    #include "lwip/ip4_napt.h"
    #endif
}

// Static instance pointer for callbacks
static SLIPServer* g_slipServer = nullptr;

// Callback for receiving packets destined for the SLIP interface
static u8_t slip_recv_callback(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    if (g_slipServer && p) {
        // This packet should go to the SLIP peer
        // Copy data and send via SLIP
        uint8_t* data = (uint8_t*)malloc(p->tot_len);
        if (data) {
            pbuf_copy_partial(p, data, p->tot_len, 0);
            g_slipServer->sendPacket(data, p->tot_len);
            free(data);
        }
    }
    // Don't consume the packet
    return 0;
}

SLIPServer::SLIPServer(HardwareSerial& serial)
    : _serial(serial)
    , _state(SLIP_STATE_IDLE)
    , _rxLen(0)
    , _escaping(false)
    , _bytesSent(0)
    , _bytesRecv(0)
    , _packetsSent(0)
    , _packetsRecv(0)
{
    g_slipServer = this;
}

SLIPServer::~SLIPServer() {
    stop();
    g_slipServer = nullptr;
}

bool SLIPServer::start() {
    if (_state != SLIP_STATE_IDLE) {
        return false;
    }

    // Reset state
    _rxLen = 0;
    _escaping = false;
    _bytesSent = 0;
    _bytesRecv = 0;
    _packetsSent = 0;
    _packetsRecv = 0;

    // Enable NAPT on WiFi interface
#if IP_NAPT
    if (WiFi.status() == WL_CONNECTED) {
        ip_napt_enable(WiFi.localIP(), 1);
        Serial.println("NAPT enabled");
    }
#endif

    _state = SLIP_STATE_ACTIVE;

    // Send initial END to flush any garbage
    _serial.write(SLIP_END);

    return true;
}

void SLIPServer::stop() {
    if (_state == SLIP_STATE_IDLE) {
        return;
    }

#if IP_NAPT
    ip_napt_enable(WiFi.localIP(), 0);
#endif

    _state = SLIP_STATE_IDLE;
    _rxLen = 0;
    _escaping = false;
}

void SLIPServer::process() {
    if (_state != SLIP_STATE_ACTIVE) {
        return;
    }

    // Read and decode SLIP frames from serial
    while (_serial.available()) {
        uint8_t c = _serial.read();
        _bytesRecv++;

        if (_escaping) {
            // Previous byte was ESC
            _escaping = false;
            if (c == SLIP_ESC_END) {
                c = SLIP_END;
            } else if (c == SLIP_ESC_ESC) {
                c = SLIP_ESC;
            }
            // else: protocol error, but store the byte anyway

            if (_rxLen < SLIP_BUF_SIZE) {
                _rxBuffer[_rxLen++] = c;
            }
        } else if (c == SLIP_END) {
            // End of frame
            if (_rxLen > 0) {
                processReceivedPacket(_rxBuffer, _rxLen);
                _packetsRecv++;
            }
            _rxLen = 0;
        } else if (c == SLIP_ESC) {
            _escaping = true;
        } else {
            if (_rxLen < SLIP_BUF_SIZE) {
                _rxBuffer[_rxLen++] = c;
            }
        }
    }
}

void SLIPServer::sendPacket(uint8_t* data, size_t len) {
    // Send END to flush
    _serial.write(SLIP_END);

    // Send data with escaping
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == SLIP_END) {
            _serial.write(SLIP_ESC);
            _serial.write(SLIP_ESC_END);
            _bytesSent += 2;
        } else if (c == SLIP_ESC) {
            _serial.write(SLIP_ESC);
            _serial.write(SLIP_ESC_ESC);
            _bytesSent += 2;
        } else {
            _serial.write(c);
            _bytesSent++;
        }
    }

    // Send final END
    _serial.write(SLIP_END);
    _bytesSent += 2;  // Two END bytes
    _packetsSent++;
}

void SLIPServer::processReceivedPacket(uint8_t* data, size_t len) {
    // Basic IP packet validation
    if (len < 20) return;  // Minimum IP header size

    uint8_t version = (data[0] >> 4) & 0x0F;
    if (version != 4) return;  // Only IPv4 for now

    uint8_t ihl = data[0] & 0x0F;
    if (ihl < 5) return;  // Invalid header length

    uint16_t totalLen = (data[2] << 8) | data[3];
    if (totalLen > len) return;  // Truncated packet

    // Forward to WiFi interface
    forwardToWifi(data, len);
}

void SLIPServer::forwardToWifi(uint8_t* ipPacket, size_t len) {
    // Create pbuf for the packet
    struct pbuf* p = pbuf_alloc(PBUF_IP, len, PBUF_RAM);
    if (p == nullptr) {
        return;
    }

    // Copy packet data
    memcpy(p->payload, ipPacket, len);

    // Get source and destination from IP header
    uint32_t srcIp = (ipPacket[12] << 24) | (ipPacket[13] << 16) | (ipPacket[14] << 8) | ipPacket[15];
    uint32_t dstIp = (ipPacket[16] << 24) | (ipPacket[17] << 16) | (ipPacket[18] << 8) | ipPacket[19];

    // For NAT/routing, we need to:
    // 1. Modify source IP to our WiFi IP (so responses come back to us)
    // 2. Track the mapping so we can route responses back

    // Get WiFi interface
    struct netif* wifi_netif = netif_default;
    if (wifi_netif == nullptr) {
        pbuf_free(p);
        return;
    }

    // Simple NAT: Replace source IP with our WiFi IP
    // This is a simplified approach - proper NAPT tracks ports too
    IPAddress wifiIp = WiFi.localIP();
    ipPacket[12] = wifiIp[0];
    ipPacket[13] = wifiIp[1];
    ipPacket[14] = wifiIp[2];
    ipPacket[15] = wifiIp[3];

    // Recalculate IP checksum
    // Clear old checksum
    ipPacket[10] = 0;
    ipPacket[11] = 0;

    // Calculate new checksum
    uint32_t sum = 0;
    uint8_t ihl = (ipPacket[0] & 0x0F) * 4;
    for (int i = 0; i < ihl; i += 2) {
        sum += (ipPacket[i] << 8) | ipPacket[i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    uint16_t checksum = ~sum;
    ipPacket[10] = (checksum >> 8) & 0xFF;
    ipPacket[11] = checksum & 0xFF;

    // Update pbuf with modified packet
    memcpy(p->payload, ipPacket, len);

    // Send through WiFi interface
    // Using low-level output to bypass routing
    ip4_addr_t dest;
    IP4_ADDR(&dest,
        (dstIp >> 24) & 0xFF,
        (dstIp >> 16) & 0xFF,
        (dstIp >> 8) & 0xFF,
        dstIp & 0xFF);

    // Output packet through the interface
    wifi_netif->output(wifi_netif, p, &dest);

    pbuf_free(p);
}
