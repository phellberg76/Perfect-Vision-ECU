#!/usr/bin/env python3
"""
ESP32 SPI Communication Library for RPi GPS Hat v4

This library communicates with the ESP32 over SPI to:
- Read GPS data
- Get status information
- Configure WiFi
- Control PPP mode

Hardware:
    CE2 (GPIO4) -> ESP32 CS (directly controlled)
    MOSI (GPIO10) -> ESP32 IO11
    MISO (GPIO9) -> ESP32 IO13
    SCLK (GPIO11) -> ESP32 IO12
    GPIO5 -> ESP32 IO3 (Handshake, active low = ready)

Usage:
    from esp32_spi import ESP32Hat

    hat = ESP32Hat()
    gps = hat.get_gps()
    print(f"Position: {gps['lat']}, {gps['lng']}")
"""

import spidev
import RPi.GPIO as GPIO
import json
import time


class ESP32Hat:
    # SPI Commands
    CMD_GET_GPS = 0x01
    CMD_GET_STATUS = 0x02
    CMD_GET_WIFI = 0x03
    CMD_SET_SSID = 0x10
    CMD_SET_PASS = 0x11
    CMD_WIFI_CONNECT = 0x12
    CMD_WIFI_DISCONNECT = 0x13
    CMD_PPP_START = 0x20
    CMD_PPP_STOP = 0x21
    CMD_PING = 0xFE
    CMD_RESET = 0xFF

    def __init__(self, bus=0, device=2, speed_hz=1000000, handshake_pin=5):
        """
        Initialize ESP32 SPI communication.

        Args:
            bus: SPI bus number (0)
            device: SPI device/CS number (2 for GPIO4)
            speed_hz: SPI clock speed (default 1MHz)
            handshake_pin: GPIO pin for handshake signal (5)
        """
        self.handshake_pin = handshake_pin

        # Setup GPIO for handshake
        GPIO.setmode(GPIO.BCM)
        GPIO.setup(self.handshake_pin, GPIO.IN, pull_up_down=GPIO.PUD_UP)

        # Setup SPI
        self.spi = spidev.SpiDev()
        self.spi.open(bus, device)
        self.spi.max_speed_hz = speed_hz
        self.spi.mode = 0

    def __del__(self):
        """Cleanup on exit."""
        try:
            self.spi.close()
            GPIO.cleanup()
        except:
            pass

    def _wait_ready(self, timeout=1.0):
        """Wait for ESP32 to be ready (handshake pin low)."""
        start = time.time()
        while GPIO.input(self.handshake_pin) == GPIO.HIGH:
            if time.time() - start > timeout:
                raise TimeoutError("ESP32 not ready")
            time.sleep(0.001)

    def _send_command(self, cmd, data=None, response_len=256):
        """
        Send command to ESP32 and receive response.

        Args:
            cmd: Command byte
            data: Optional data bytes to send after command
            response_len: Expected response length

        Returns:
            Response as bytes
        """
        self._wait_ready()

        # Build command buffer
        tx_buf = [cmd]
        if data:
            tx_buf.extend(data)

        # Pad to response_len
        tx_buf.extend([0] * (response_len - len(tx_buf)))

        # Perform SPI transaction
        rx_buf = self.spi.xfer2(tx_buf)

        return bytes(rx_buf)

    def _parse_json_response(self, response):
        """Parse JSON from response bytes."""
        # Find null terminator or end
        try:
            end = response.index(0)
            json_str = response[1:end].decode('utf-8')  # Skip command echo byte
        except ValueError:
            json_str = response[1:].decode('utf-8', errors='ignore')

        # Find JSON start
        start = json_str.find('{')
        if start == -1:
            return None

        end = json_str.rfind('}')
        if end == -1:
            return None

        return json.loads(json_str[start:end+1])

    def ping(self):
        """
        Ping ESP32 to check connectivity.

        Returns:
            True if ESP32 responds correctly
        """
        try:
            response = self._send_command(self.CMD_PING, response_len=2)
            return response[1] == 0xFE
        except Exception:
            return False

    def get_gps(self):
        """
        Get current GPS data.

        Returns:
            dict with GPS data: lat, lng, alt, speed, course, sats, hdop, fix, time
        """
        response = self._send_command(self.CMD_GET_GPS)
        return self._parse_json_response(response)

    def get_status(self):
        """
        Get ESP32 status.

        Returns:
            dict with status: uptime, gps_ok, sats, wifi, ppp
        """
        response = self._send_command(self.CMD_GET_STATUS)
        return self._parse_json_response(response)

    def get_wifi_status(self):
        """
        Get WiFi connection status.

        Returns:
            dict with wifi: connected, ssid, ip, rssi
        """
        response = self._send_command(self.CMD_GET_WIFI)
        return self._parse_json_response(response)

    def set_wifi_ssid(self, ssid):
        """
        Set WiFi SSID (stored in ESP32 NVS).

        Args:
            ssid: WiFi network name
        """
        data = list(ssid.encode('utf-8')) + [0]  # Null-terminated
        self._send_command(self.CMD_SET_SSID, data)

    def set_wifi_password(self, password):
        """
        Set WiFi password (stored in ESP32 NVS).

        Args:
            password: WiFi password
        """
        data = list(password.encode('utf-8')) + [0]
        self._send_command(self.CMD_SET_PASS, data)

    def wifi_connect(self):
        """Connect to configured WiFi network."""
        self._send_command(self.CMD_WIFI_CONNECT)

    def wifi_disconnect(self):
        """Disconnect from WiFi."""
        self._send_command(self.CMD_WIFI_DISCONNECT)

    def configure_wifi(self, ssid, password):
        """
        Configure and connect to WiFi network.

        Args:
            ssid: WiFi network name
            password: WiFi password
        """
        self.set_wifi_ssid(ssid)
        time.sleep(0.1)
        self.set_wifi_password(password)
        time.sleep(0.1)
        self.wifi_connect()

    def start_ppp(self):
        """Start PPP mode for network sharing."""
        self._send_command(self.CMD_PPP_START)

    def stop_ppp(self):
        """Stop PPP mode."""
        self._send_command(self.CMD_PPP_STOP)

    def reset(self):
        """Reset ESP32."""
        try:
            self._send_command(self.CMD_RESET)
        except:
            pass  # ESP32 will reset before responding


# Example usage and testing
if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="ESP32 Hat Control")
    parser.add_argument("command", choices=["ping", "gps", "status", "wifi",
                                            "connect", "disconnect", "ppp_start",
                                            "ppp_stop", "reset"],
                       help="Command to execute")
    parser.add_argument("--ssid", help="WiFi SSID for connect command")
    parser.add_argument("--password", help="WiFi password for connect command")
    args = parser.parse_args()

    hat = ESP32Hat()

    if args.command == "ping":
        if hat.ping():
            print("ESP32 responding OK")
        else:
            print("ESP32 not responding!")

    elif args.command == "gps":
        gps = hat.get_gps()
        if gps:
            print(json.dumps(gps, indent=2))
        else:
            print("No GPS data")

    elif args.command == "status":
        status = hat.get_status()
        if status:
            print(json.dumps(status, indent=2))
        else:
            print("No status data")

    elif args.command == "wifi":
        wifi = hat.get_wifi_status()
        if wifi:
            print(json.dumps(wifi, indent=2))
        else:
            print("No WiFi data")

    elif args.command == "connect":
        if args.ssid and args.password:
            hat.configure_wifi(args.ssid, args.password)
            print(f"Connecting to {args.ssid}...")
            time.sleep(5)
            print(json.dumps(hat.get_wifi_status(), indent=2))
        else:
            print("--ssid and --password required")

    elif args.command == "disconnect":
        hat.wifi_disconnect()
        print("WiFi disconnected")

    elif args.command == "ppp_start":
        hat.start_ppp()
        print("PPP mode started")

    elif args.command == "ppp_stop":
        hat.stop_ppp()
        print("PPP mode stopped")

    elif args.command == "reset":
        hat.reset()
        print("ESP32 reset")
