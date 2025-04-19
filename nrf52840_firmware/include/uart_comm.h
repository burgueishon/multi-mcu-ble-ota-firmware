#ifndef UART_COMM_H
#define UART_COMM_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>

//
// UART packet types and OTA command codes
//

#define PREAMBLE 0xAA // Start byte for UART packets

// UART packet types
#define UART_PKT_SENSOR 0x01 // Sensor data packet
#define UART_PKT_CMD 0x02    // Command packet
#define UART_PKT_ACK 0x03    // Acknowledgment packet

// OTA command types
#define OTA_CMD_INIT 0x00   // Begin OTA
#define OTA_CMD_DATA 0x01   // OTA data chunk
#define OTA_CMD_END 0x02    // End OTA
#define OTA_CMD_REBOOT 0x03 // Reboot device

// Power management commands
#define UART_CMD_SLEEP 0x10 // Put MCU to sleep
#define UART_CMD_WAKE 0x11  // Wake MCU (no-op on target)

#define UART_MAX_PAYLOAD_SIZE 64 // Max bytes in UART payload

//
// UART packet structure
//
typedef struct {
  uint8_t preamble;                       // Should always be 0xAA
  uint8_t type;                           // Packet type
  uint8_t length;                         // Payload length
  uint8_t payload[UART_MAX_PAYLOAD_SIZE]; // Payload data
  uint8_t crc; // CRC8 of entire packet (excluding this byte)
} __packed uart_packet_t;

//
// Sensor data structure
//
typedef struct {
  float temperature; // Temperature in °C
  float humidity;    // Relative humidity in %
  bool valid;        // Flag indicating if values are valid
} sensor_data_t;

//
// UART communication API
//
int uart_comm_init(void);
const sensor_data_t *uart_comm_get_sensor_data(void);
const struct device *uart_comm_get_dev(void);
int uart_comm_send_cmd(uint8_t pkt_type, uint8_t cmd, const uint8_t *payload,
                       uint8_t length);
bool uart_comm_is_tx_busy(void);

#endif // UART_COMM_H
