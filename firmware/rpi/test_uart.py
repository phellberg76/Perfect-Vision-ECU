#!/usr/bin/env python3
"""
Simple UART test for ESP32 communication before SPI is set up.

This script communicates with the ESP32 via /dev/ttyAMA0 using
the text command interface (available when PPP mode is not active).

Usage:
    python3 test_uart.py [command]

Commands:
    STATUS       - Get device status
    GPS          - Get GPS data
    WIFI         - Get WiFi status
    WIFI_SSID x  - Set WiFi SSID
    WIFI_PASS x  - Set WiFi password
    WIFI_CONNECT - Connect to WiFi
    PPP_START    - Start PPP mode (warning: disables text commands)
    RESET        - Reset ESP32
"""

import serial
import sys
import time

SERIAL_PORT = "/dev/ttyAMA0"
BAUD_RATE = 115200

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 test_uart.py <command>")
        print("Commands: STATUS, GPS, WIFI, WIFI_SSID <ssid>, WIFI_PASS <pass>,")
        print("          WIFI_CONNECT, PPP_START, RESET")
        return

    command = " ".join(sys.argv[1:])

    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=2)

        # Clear any pending data
        ser.reset_input_buffer()

        # Send command
        print(f"Sending: {command}")
        ser.write((command + "\n").encode())
        ser.flush()

        # Wait for response
        time.sleep(0.5)

        # Read response
        response = ""
        while ser.in_waiting:
            response += ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            time.sleep(0.1)

        if response:
            print(f"Response:\n{response}")
        else:
            print("No response received")

        ser.close()

    except serial.SerialException as e:
        print(f"Serial error: {e}")
        print("Make sure /dev/ttyAMA0 is available and ESP32 is connected")

if __name__ == "__main__":
    main()
