/**
 * @file pin_config.h
 * @brief GPIO pin definitions for Perfect Vision ECU
 *
 * Pin assignments based on hardware schematic Perfect-Vision-ECU.kicad_sch
 * Adjust these values if the hardware layout differs.
 */

#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

#include "driver/gpio.h"

// =============================================================================
// CAN0 - Internal TWAI Controller (connected to SN65HVD230 transceiver)
// Hardware: GPIO5 -> U7.1 (D/TX), GPIO4 -> U7.4 (R/RX)
// =============================================================================
#define CAN0_TX_GPIO        GPIO_NUM_5
#define CAN0_RX_GPIO        GPIO_NUM_4

// =============================================================================
// CAN1 - MCP2515 External SPI CAN Controller
// =============================================================================
#define MCP2515_SPI_HOST    SPI2_HOST
#define MCP2515_MOSI_GPIO   GPIO_NUM_11
#define MCP2515_MISO_GPIO   GPIO_NUM_13
#define MCP2515_CLK_GPIO    GPIO_NUM_12
#define MCP2515_CS_GPIO     GPIO_NUM_10
#define MCP2515_INT_GPIO    GPIO_NUM_9

// MCP2515 crystal frequency (8 MHz on board)
#define MCP2515_CRYSTAL_FREQ_HZ     8000000

// SPI clock frequency for MCP2515 (max 10 MHz)
#define MCP2515_SPI_FREQ_HZ         8000000

// =============================================================================
// CAN Channel Relay Control
// =============================================================================
#define RELAY_CONTROL_GPIO  GPIO_NUM_8

// =============================================================================
// Status LED (optional, adjust based on hardware)
// =============================================================================
// #define STATUS_LED_GPIO     GPIO_NUM_XX

// =============================================================================
// GPS Module (NEO-M9N)
// =============================================================================
// NOTE: Using UART1 instead of UART0 to avoid conflict with console output.
//
// HARDWARE VERSION NOTE:
// ----------------------
// Rev 1.0 PCB has RX/TX crossed twice in the schematic, resulting in:
//   - ESP32 GPIO43 connected to NEO-M9N TXD (GPS transmit)
//   - ESP32 GPIO44 connected to NEO-M9N RXD (GPS receive)
// This is opposite to the silk/net names. The pin assignments below are
// correct for Rev 1.0 hardware.
//
// TODO: For Rev 2.0 hardware (with corrected RX/TX routing), swap these back:
//   #define GPS_TX_GPIO  GPIO_NUM_43  // ESP32 TX to GPS RX
//   #define GPS_RX_GPIO  GPIO_NUM_44  // ESP32 RX from GPS TX
//
// NEO-M9N Pin Notes:
// - D_SEL: Must be HIGH or floating for UART mode (R18 pulls high)
// - EXTINT: Can be left floating for normal operation. Only needed for
//   waking from power save modes. Does NOT need to be driven for GPS to run.
// - RESET_N: Must be HIGH or floating (internal pull-up)
// - SAFEBOOT_N: Must be HIGH or floating for normal operation
//
#define GPS_UART_NUM        UART_NUM_1
#define GPS_TX_GPIO         GPIO_NUM_44  // ESP32 TX to GPS RX (Rev 1.0 PCB)
#define GPS_RX_GPIO         GPIO_NUM_43  // ESP32 RX from GPS TX (Rev 1.0 PCB)
#define GPS_BAUD_RATE       38400        // NEO-M9N default baud rate
#define GPS_LNA_EN_GPIO     GPIO_NUM_38  // LNA enable for active antenna power

#endif // PIN_CONFIG_H
