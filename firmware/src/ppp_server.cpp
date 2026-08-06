/*
 * PPP Server Implementation
 *
 * Implements minimal PPP server for network sharing:
 * - HDLC-like framing (0x7E delimiters)
 * - LCP negotiation
 * - IPCP negotiation
 * - IP packet forwarding via lwIP
 */

#include "ppp_server.h"
#include "esp_netif.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/ip.h"
#include "lwip/ip4.h"
#include <WiFi.h>

// NAPT support (if available)
#if IP_NAPT
extern "C" {
    #include "lwip/ip4_napt.h"
}
#endif

// HDLC-like framing
#define PPP_FLAG     0x7E
#define PPP_ESCAPE   0x7D
#define PPP_TRANS    0x20

// PPP Protocol IDs
#define PPP_IP       0x0021
#define PPP_LCP      0xC021
#define PPP_IPCP     0x8021
#define PPP_IPV6CP   0x8057
#define PPP_CCP      0x80FD

// LCP Protocol-Reject code
#define PROT_REJ     8

// LCP/IPCP codes
#define CONF_REQ     1
#define CONF_ACK     2
#define CONF_NAK     3
#define CONF_REJ     4
#define TERM_REQ     5
#define TERM_ACK     6
#define ECHO_REQ     9
#define ECHO_REP     10

// LCP Options
#define LCP_OPT_MRU          1
#define LCP_OPT_ACCM         2
#define LCP_OPT_AUTH         3
#define LCP_OPT_MAGIC        5
#define LCP_OPT_PFC          7
#define LCP_OPT_ACFC         8

// IPCP Options
#define IPCP_OPT_IPADDR      3
#define IPCP_OPT_DNS1        129
#define IPCP_OPT_DNS2        131

// FCS (Frame Check Sequence) - CRC-16-CCITT lookup table
static const uint16_t fcstab[256] = {
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
    0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
    0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
    0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
    0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
    0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
    0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
    0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
    0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
    0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
    0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
    0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
    0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
    0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
    0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
    0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
    0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
    0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
    0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
    0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
    0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
    0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
    0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
    0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
    0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
    0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
    0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
    0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
    0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

static uint16_t pppfcs16(uint16_t fcs, uint8_t *cp, int len) {
    while (len--) {
        fcs = (fcs >> 8) ^ fcstab[(fcs ^ *cp++) & 0xff];
    }
    return fcs;
}

PPPServer::PPPServer(HardwareSerial& serial)
    : _serial(serial)
    , _state(PPP_STATE_IDLE)
    , _pppNetif(nullptr)
    , _bytesSent(0)
    , _bytesRecv(0)
    , _rxLen(0)
    , _escapeNext(false)
{
}

PPPServer::~PPPServer() {
    stop();
}

bool PPPServer::init() {
    // Initialize netif for PPP
    // Note: Full implementation would create esp_netif here
    return true;
}

bool PPPServer::start() {
    if (_state != PPP_STATE_IDLE) {
        Serial.println("[PPP] Already started");
        return false;
    }

    Serial.println("[PPP] Starting PPP server...");

    _state = PPP_STATE_STARTING;
    _rxLen = 0;
    _escapeNext = false;

    // Enable NAPT for WiFi interface
    // This allows packets from PPP to be NAT'd to WiFi
#if IP_NAPT
    ip_napt_enable(WiFi.localIP(), 1);
    Serial.println("[PPP] NAPT enabled");
#endif

    // Send initial LCP Configure-Request
    Serial.println("[PPP] Sending initial LCP Config-Req");
    sendLcpConfigReq();

    return true;
}

void PPPServer::stop() {
    if (_state == PPP_STATE_IDLE) {
        return;
    }

    // Disable NAPT
#if IP_NAPT
    ip_napt_enable(WiFi.localIP(), 0);
#endif

    _state = PPP_STATE_IDLE;
    _rxLen = 0;
}

void PPPServer::process() {
    if (_state == PPP_STATE_IDLE) {
        return;
    }

    // Read incoming data
    while (_serial.available()) {
        uint8_t b = _serial.read();

        if (_escapeNext) {
            // Previous byte was escape
            _escapeNext = false;
            b ^= PPP_TRANS;
            if (_rxLen < sizeof(_rxBuffer)) {
                _rxBuffer[_rxLen++] = b;
            }
        } else if (b == PPP_FLAG) {
            if (_rxLen > 0) {
                // End of frame
                size_t frameLen = _rxLen;
                _rxLen = 0;

                // Verify FCS - good FCS over data+fcs should equal 0xF0B8
                if (frameLen >= 4) {
                    uint16_t fcs = pppfcs16(0xFFFF, _rxBuffer, frameLen);
                    if (fcs == 0xF0B8) {
                        // Remove FCS from payload
                        processFrame(_rxBuffer, frameLen - 2);
                    } else {
                        // Debug: bad FCS
                        Serial.printf("[PPP] Bad FCS: 0x%04X (len=%d)\n", fcs, frameLen);
                    }
                }
            }
            // Either way, this is start of new frame
        } else if (b == PPP_ESCAPE) {
            // Next byte is escaped
            _escapeNext = true;
        } else {
            if (_rxLen < sizeof(_rxBuffer)) {
                _rxBuffer[_rxLen++] = b;
            }
        }
    }
}

void PPPServer::sendPppFrame(uint8_t* data, size_t len) {
    // Build complete frame with Address and Control for FCS calculation
    // PPP FCS must be calculated over: Address + Control + Protocol + Information
    uint8_t frameWithAC[258];  // Max frame size
    frameWithAC[0] = 0xFF;     // All-stations address
    frameWithAC[1] = 0x03;     // UI control
    memcpy(frameWithAC + 2, data, len);
    size_t totalLen = len + 2;

    // Calculate FCS over Address + Control + data
    uint16_t fcs = pppfcs16(0xFFFF, frameWithAC, totalLen);
    fcs ^= 0xFFFF;

    // Send flag
    _serial.write(PPP_FLAG);

    // Helper to send byte with escaping
    auto sendEscaped = [this](uint8_t b) {
        if (b < 0x20 || b == PPP_FLAG || b == PPP_ESCAPE) {
            _serial.write(PPP_ESCAPE);
            _serial.write(b ^ PPP_TRANS);
        } else {
            _serial.write(b);
        }
    };

    // Send Address, Control, and data with escaping
    for (size_t i = 0; i < totalLen; i++) {
        sendEscaped(frameWithAC[i]);
    }

    // Send FCS (little-endian) with escaping
    sendEscaped(fcs & 0xFF);
    sendEscaped((fcs >> 8) & 0xFF);

    // Send flag
    _serial.write(PPP_FLAG);

    _bytesSent += len;
}

void PPPServer::processFrame(uint8_t* data, size_t len) {
    if (len < 4) return;  // Minimum: addr + ctrl + protocol(2)

    _bytesRecv += len;

    Serial.printf("[PPP] Frame received: %d bytes\n", len);

    // Skip address (0xFF) and control (0x03) if present
    size_t offset = 0;
    if (data[0] == 0xFF && data[1] == 0x03) {
        offset = 2;
    }

    // Get protocol
    uint16_t protocol;
    if (data[offset] & 0x01) {
        // Compressed protocol field (single byte)
        protocol = data[offset];
        offset += 1;
    } else {
        // Full protocol field
        protocol = (data[offset] << 8) | data[offset + 1];
        offset += 2;
    }

    uint8_t* payload = data + offset;
    size_t payloadLen = len - offset;

    Serial.printf("[PPP] Protocol: 0x%04X\n", protocol);

    switch (protocol) {
        case PPP_LCP:
            handleLcp(payload, payloadLen);
            break;
        case PPP_IPCP:
            handleIpcp(payload, payloadLen);
            break;
        case PPP_IP:
            handleIp(payload, payloadLen);
            break;
        case PPP_CCP:
        case PPP_IPV6CP:
            // Reject unsupported protocols with LCP Protocol-Reject
            Serial.printf("[PPP] Rejecting protocol 0x%04X\n", protocol);
            sendProtocolReject(protocol, payload, payloadLen);
            break;
        default:
            Serial.printf("[PPP] Unknown protocol: 0x%04X\n", protocol);
            break;
    }
}

void PPPServer::handleLcp(uint8_t* data, size_t len) {
    if (len < 4) return;

    uint8_t code = data[0];
    uint8_t id = data[1];
    uint16_t length = (data[2] << 8) | data[3];

    Serial.printf("[PPP] LCP: code=%d id=%d len=%d\n", code, id, length);

    switch (code) {
        case CONF_REQ:
            // Peer is requesting configuration - ACK everything for simplicity
            Serial.println("[PPP] LCP: Received Config-Req, sending Config-Ack");
            sendLcpConfigAck(id, data + 4, length - 4);
            // Also send our own Config-Req (in case initial one was missed)
            if (_state == PPP_STATE_STARTING) {
                Serial.println("[PPP] LCP: Sending our Config-Req");
                sendLcpConfigReq();
            }
            break;

        case CONF_ACK:
            // Our config was accepted - move to IPCP
            Serial.println("[PPP] LCP: Received Config-Ack, moving to IPCP");
            _state = PPP_STATE_LCP_OPEN;
            sendIpcpConfigReq();
            break;

        case CONF_NAK:
        case CONF_REJ:
            // Try again with different options
            sendLcpConfigReq();
            break;

        case TERM_REQ:
            // Peer wants to terminate
            {
                uint8_t termAck[4] = {TERM_ACK, id, 0, 4};
                uint8_t frame[6] = {0xC0, 0x21};  // LCP protocol
                memcpy(frame + 2, termAck, 4);
                sendPppFrame(frame, 6);
            }
            _state = PPP_STATE_IDLE;
            break;

        case ECHO_REQ:
            // Respond to echo
            {
                uint8_t echoReply[8];
                echoReply[0] = ECHO_REP;
                echoReply[1] = id;
                echoReply[2] = 0;
                echoReply[3] = 8;
                // Magic number
                echoReply[4] = 0;
                echoReply[5] = 0;
                echoReply[6] = 0;
                echoReply[7] = 0;

                uint8_t frame[10] = {0xC0, 0x21};
                memcpy(frame + 2, echoReply, 8);
                sendPppFrame(frame, 10);
            }
            break;
    }
}

void PPPServer::handleIpcp(uint8_t* data, size_t len) {
    if (len < 4) return;

    uint8_t code = data[0];
    uint8_t id = data[1];
    uint16_t length = (data[2] << 8) | data[3];

    switch (code) {
        case CONF_REQ:
            // Peer requesting IP config
            // Check options and potentially NAK with our IP assignment
            {
                uint8_t* options = data + 4;
                size_t optLen = length - 4;
                bool needNak = false;
                uint8_t nakBuf[64];
                size_t nakLen = 0;

                // Parse options
                size_t pos = 0;
                while (pos < optLen) {
                    uint8_t optType = options[pos];
                    uint8_t optLength = options[pos + 1];

                    if (optType == IPCP_OPT_IPADDR) {
                        // Check if peer is requesting 0.0.0.0 (wants us to assign)
                        uint32_t reqIp = (options[pos+2] << 24) |
                                        (options[pos+3] << 16) |
                                        (options[pos+4] << 8) |
                                        options[pos+5];
                        if (reqIp == 0 || reqIp != 0x0A000002) {  // Not 10.0.0.2
                            // NAK with our assigned IP
                            needNak = true;
                            nakBuf[nakLen++] = IPCP_OPT_IPADDR;
                            nakBuf[nakLen++] = 6;
                            nakBuf[nakLen++] = 10;  // 10.0.0.2
                            nakBuf[nakLen++] = 0;
                            nakBuf[nakLen++] = 0;
                            nakBuf[nakLen++] = 2;
                        }
                    }

                    pos += optLength;
                }

                if (needNak) {
                    // Send CONF_NAK with our suggestions
                    uint8_t frame[64] = {0x80, 0x21};  // IPCP protocol
                    frame[2] = CONF_NAK;
                    frame[3] = id;
                    frame[4] = ((nakLen + 4) >> 8) & 0xFF;
                    frame[5] = (nakLen + 4) & 0xFF;
                    memcpy(frame + 6, nakBuf, nakLen);
                    sendPppFrame(frame, 6 + nakLen);
                } else {
                    // ACK the config
                    sendIpcpConfigAck(id, options, optLen);
                }
            }
            break;

        case CONF_ACK:
            // Our IPCP config was accepted - we're connected!
            _state = PPP_STATE_CONNECTED;
            break;

        case CONF_NAK:
            // Peer is suggesting different values - accept them
            sendIpcpConfigReq();
            break;
    }
}

void PPPServer::handleIp(uint8_t* data, size_t len) {
    if (len < 20) return;  // Minimum IP header

    // Parse IP header
    uint8_t protocol = data[9];
    uint32_t dstIp = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];

    Serial.printf("[PPP] IP packet: proto=%d dst=%d.%d.%d.%d len=%d\n",
        protocol, data[16], data[17], data[18], data[19], len);

    // Check if destination is us (10.0.0.1 = 0x0A000001)
    if (dstIp == 0x0A000001) {
        // Packet is for us
        if (protocol == 1) {  // ICMP
            handleIcmp(data, len);
        } else {
            Serial.printf("[PPP] Ignoring protocol %d to us\n", protocol);
        }
        return;
    }

    // Forward to WiFi interface
    Serial.println("[PPP] Forwarding to WiFi...");

    // Create pbuf for the packet
    struct pbuf* p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
    if (p == nullptr) {
        Serial.println("[PPP] Failed to allocate pbuf");
        return;
    }

    memcpy(p->payload, data, len);

    // Get the WiFi netif
    struct netif* wifi_netif = netif_default;
    if (wifi_netif == nullptr) {
        Serial.println("[PPP] No default netif");
        pbuf_free(p);
        return;
    }

    // Forward the packet
#if LWIP_IPV4
    ip4_input(p, wifi_netif);
#else
    pbuf_free(p);
#endif
}

void PPPServer::handleIcmp(uint8_t* data, size_t len) {
    // IP header length
    uint8_t ihl = (data[0] & 0x0F) * 4;
    if (len < ihl + 8) return;

    uint8_t* icmp = data + ihl;
    uint8_t type = icmp[0];
    uint8_t code = icmp[1];

    Serial.printf("[PPP] ICMP: type=%d code=%d\n", type, code);

    if (type == 8 && code == 0) {
        // Echo Request - send Echo Reply
        Serial.println("[PPP] Responding to ping");

        // Swap source and destination IP
        uint8_t tmpIp[4];
        memcpy(tmpIp, data + 12, 4);       // Save src
        memcpy(data + 12, data + 16, 4);   // dst -> src
        memcpy(data + 16, tmpIp, 4);       // saved src -> dst

        // Change ICMP type to Echo Reply (0)
        icmp[0] = 0;

        // Recalculate ICMP checksum
        icmp[2] = 0;
        icmp[3] = 0;
        uint32_t sum = 0;
        size_t icmpLen = len - ihl;
        for (size_t i = 0; i < icmpLen; i += 2) {
            if (i + 1 < icmpLen) {
                sum += (icmp[i] << 8) | icmp[i + 1];
            } else {
                sum += icmp[i] << 8;
            }
        }
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        uint16_t checksum = ~sum;
        icmp[2] = (checksum >> 8) & 0xFF;
        icmp[3] = checksum & 0xFF;

        // Recalculate IP checksum
        data[10] = 0;
        data[11] = 0;
        sum = 0;
        for (size_t i = 0; i < ihl; i += 2) {
            sum += (data[i] << 8) | data[i + 1];
        }
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        checksum = ~sum;
        data[10] = (checksum >> 8) & 0xFF;
        data[11] = checksum & 0xFF;

        // Send as PPP IP packet
        uint8_t frame[1600];
        frame[0] = 0x00;  // IP protocol high byte
        frame[1] = 0x21;  // IP protocol low byte
        memcpy(frame + 2, data, len);
        sendPppFrame(frame, len + 2);

        Serial.println("[PPP] Ping reply sent");
    }
}

void PPPServer::sendLcpConfigReq() {
    // LCP Config-Request with basic options
    uint8_t frame[32];
    size_t pos = 0;

    // Protocol
    frame[pos++] = 0xC0;
    frame[pos++] = 0x21;

    // Code: Configure-Request
    frame[pos++] = CONF_REQ;

    // Identifier
    static uint8_t lcpId = 1;
    frame[pos++] = lcpId++;

    // Length placeholder
    size_t lenPos = pos;
    frame[pos++] = 0;
    frame[pos++] = 0;

    // Options

    // MRU: 1500
    frame[pos++] = LCP_OPT_MRU;
    frame[pos++] = 4;
    frame[pos++] = 0x05;  // 1500 >> 8
    frame[pos++] = 0xDC;  // 1500 & 0xFF

    // Magic number (for loop detection)
    frame[pos++] = LCP_OPT_MAGIC;
    frame[pos++] = 6;
    frame[pos++] = 0x12;
    frame[pos++] = 0x34;
    frame[pos++] = 0x56;
    frame[pos++] = 0x78;

    // Fill in length
    uint16_t len = pos - 2;  // Exclude protocol bytes
    frame[lenPos] = (len >> 8) & 0xFF;
    frame[lenPos + 1] = len & 0xFF;

    sendPppFrame(frame, pos);
}

void PPPServer::sendLcpConfigAck(uint8_t id, uint8_t* options, size_t optLen) {
    uint8_t frame[256];
    size_t pos = 0;

    // Protocol
    frame[pos++] = 0xC0;
    frame[pos++] = 0x21;

    // Code: Configure-Ack
    frame[pos++] = CONF_ACK;
    frame[pos++] = id;

    // Length
    uint16_t len = optLen + 4;
    frame[pos++] = (len >> 8) & 0xFF;
    frame[pos++] = len & 0xFF;

    // Echo back the options
    memcpy(frame + pos, options, optLen);
    pos += optLen;

    sendPppFrame(frame, pos);
}

void PPPServer::sendIpcpConfigReq() {
    uint8_t frame[32];
    size_t pos = 0;

    // Protocol
    frame[pos++] = 0x80;
    frame[pos++] = 0x21;

    // Code: Configure-Request
    frame[pos++] = CONF_REQ;

    // Identifier
    static uint8_t ipcpId = 1;
    frame[pos++] = ipcpId++;

    // Length placeholder
    size_t lenPos = pos;
    frame[pos++] = 0;
    frame[pos++] = 0;

    // IP Address option: 10.0.0.1 (our address)
    frame[pos++] = IPCP_OPT_IPADDR;
    frame[pos++] = 6;
    frame[pos++] = 10;
    frame[pos++] = 0;
    frame[pos++] = 0;
    frame[pos++] = 1;

    // Fill in length
    uint16_t len = pos - 2;
    frame[lenPos] = (len >> 8) & 0xFF;
    frame[lenPos + 1] = len & 0xFF;

    sendPppFrame(frame, pos);
}

void PPPServer::sendProtocolReject(uint16_t protocol, uint8_t* data, size_t len) {
    uint8_t frame[256];
    size_t pos = 0;

    // LCP Protocol
    frame[pos++] = 0xC0;
    frame[pos++] = 0x21;

    // Code: Protocol-Reject
    frame[pos++] = PROT_REJ;

    // Identifier
    static uint8_t rejId = 1;
    frame[pos++] = rejId++;

    // Length (will fill in later)
    size_t lenPos = pos;
    pos += 2;

    // Rejected protocol
    frame[pos++] = (protocol >> 8) & 0xFF;
    frame[pos++] = protocol & 0xFF;

    // Copy some of the rejected packet (up to reasonable size)
    size_t copyLen = (len > 64) ? 64 : len;
    memcpy(frame + pos, data, copyLen);
    pos += copyLen;

    // Fill in length
    uint16_t pktLen = pos - 2;  // Exclude protocol bytes
    frame[lenPos] = (pktLen >> 8) & 0xFF;
    frame[lenPos + 1] = pktLen & 0xFF;

    sendPppFrame(frame, pos);
}

void PPPServer::sendIpcpConfigAck(uint8_t id, uint8_t* options, size_t optLen) {
    uint8_t frame[256];
    size_t pos = 0;

    // Protocol
    frame[pos++] = 0x80;
    frame[pos++] = 0x21;

    // Code: Configure-Ack
    frame[pos++] = CONF_ACK;
    frame[pos++] = id;

    // Length
    uint16_t len = optLen + 4;
    frame[pos++] = (len >> 8) & 0xFF;
    frame[pos++] = len & 0xFF;

    // Echo back the options
    memcpy(frame + pos, options, optLen);
    pos += optLen;

    sendPppFrame(frame, pos);
}
