/*
 * pins.h - Hardware Pin Definitions
 *
 * Raspberry Pi Backup Power and GPS Hat v4
 * ESP32-S3-MINI-1 Module
 *
 * Role: GPS data acquisition and relay to Raspberry Pi
 *
 * IMPORTANT: Verify pin assignments against your schematic.
 */

#ifndef PINS_H
#define PINS_H

// =============================================================================
// GPS - u-blox NEO-M9N Module (UART1)
// =============================================================================
#define PIN_GPS_TX      17      // ESP32 TX -> GPS RX
#define PIN_GPS_RX      18      // ESP32 RX <- GPS TX
#define PIN_GPS_LNA_EN  8       // LNA Enable (active high)

#define GPS_UART_NUM    1
#define GPS_BAUD_RATE   9600

// =============================================================================
// RPi Communication (USB-CDC via USB-C connector)
// =============================================================================
// USB serial is used for bidirectional communication with the RPi
// No explicit pins needed - uses native USB

// =============================================================================
// Status LED
// =============================================================================
#define PIN_LED         4

#endif // PINS_H
