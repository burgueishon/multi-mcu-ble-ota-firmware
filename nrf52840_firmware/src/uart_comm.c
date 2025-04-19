#include "uart_comm.h"

#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include "ota_service.h"

LOG_MODULE_REGISTER(uart_comm, LOG_LEVEL_INF);

#define UART_NODE DT_NODELABEL(uart0)
#define BUF_SIZE 256

static const struct device *uart_dev = DEVICE_DT_GET(UART_NODE);
static uint8_t rx_buf[BUF_SIZE];
static size_t rx_len = 0;
static volatile bool uart_tx_busy = false;

// Latest valid sensor data received
static sensor_data_t latest_sensor_data = {
    .temperature = 0.0f,
    .humidity = 0.0f,
    .valid = false,
};

// Return pointer to latest sensor data
const sensor_data_t *uart_comm_get_sensor_data(void) {
  return &latest_sensor_data;
}

// Return TX status flag
bool uart_comm_is_tx_busy(void) { return uart_tx_busy; }

// External handler for ACKs received over UART
extern void ota_service_handle_ack(uint8_t *payload, uint8_t length);

// Send command packet over UART
int uart_comm_send_cmd(uint8_t pkt_type, uint8_t cmd, const uint8_t *payload,
                       uint8_t length) {
  uint8_t buf[3 + 1 + UART_MAX_PAYLOAD_SIZE]; // preamble + type + length + cmd
                                              // + payload + crc
  size_t total_payload_len = length;
  size_t total_len;

  buf[0] = PREAMBLE;
  buf[1] = pkt_type;

  if (pkt_type == UART_PKT_CMD || pkt_type == UART_PKT_ACK) {
    total_payload_len += 1;
    buf[2] = total_payload_len;
    buf[3] = cmd;
    if (payload && length > 0) {
      memcpy(&buf[4], payload, length);
    }
    total_len = 3 + total_payload_len + 1;
  } else {
    buf[2] = length;
    if (payload && length > 0) {
      memcpy(&buf[3], payload, length);
    }
    total_len = 3 + length + 1;
  }

  uint8_t crc = crc8_ccitt(0xFF, buf, total_len - 1);
  buf[total_len - 1] = crc;

  for (size_t i = 0; i < total_len; i++) {
    uart_poll_out(uart_dev, buf[i]);
  }

  LOG_INF("UART TX sent (%d bytes)", total_len);
  return 0;
}

// Debug utility (currently disabled)
static void print_hex_buffer(const uint8_t *buf, size_t len) {
  char hex_str[3 * BUF_SIZE] = {0};
  char *p = hex_str;

  for (size_t i = 0; i < len; i++) {
    p += snprintf(p, sizeof(hex_str) - (p - hex_str), "%02X ", buf[i]);
  }

  // Disabled to save memory
  // printk("UART RX (%d bytes): %s\n", (int)len, hex_str);
}

// OTA subcommands
static void ota_service_start(void) {
  LOG_INF("OTA INIT received");
  ota_active = true;
  ota_phase = OTA_PHASE_IDLE;
}

static void ota_service_receive_chunk(const uint8_t *data, uint8_t len) {
  if (!ota_active || ota_phase != OTA_PHASE_SENDING_DATA) {
    LOG_WRN("OTA chunk out of phase");
    return;
  }

  LOG_INF("OTA chunk received (%d bytes)", len);
  ota_queue_push(data, len);
}

static void ota_service_finalize(void) {
  if (!ota_active) {
    LOG_WRN("OTA END received without active session");
    return;
  }

  LOG_INF("OTA END received. Waiting for CRC32...");
  ota_phase = OTA_PHASE_WAIT_ACK_END;
}

// Parse and dispatch incoming UART packet
static void handle_packet_raw(const uint8_t *data, size_t len) {
  if (len < 4) {
    LOG_WRN("Packet too short");
    return;
  }

  if (data[0] != PREAMBLE) {
    LOG_WRN("Invalid preamble: 0x%02X", data[0]);
    return;
  }

  uint8_t type = data[1];
  uint8_t length = data[2];

  if ((3 + length + 1) > len) {
    LOG_WRN("Unexpected packet length: expected %d, got %d", 3 + length + 1,
            (int)len);
    return;
  }

  const uint8_t *payload = &data[3];
  uint8_t recv_crc = data[3 + length];
  uint8_t calc_crc = crc8_ccitt(0xFF, data, 3 + length);

  if (calc_crc != recv_crc) {
    LOG_WRN("CRC mismatch (calc=0x%02X, recv=0x%02X)", calc_crc, recv_crc);
    return;
  }

  LOG_INF("Valid packet received: type=0x%02X, len=%d", type, length);

  switch (type) {
  case UART_PKT_SENSOR:
    if (ota_is_active()) {
      LOG_DBG("Ignoring SENSOR packet during OTA");
      return;
    }

    if (length == 4) {
      int16_t temp_raw = (payload[0] << 8) | payload[1];
      int16_t hum_raw = (payload[2] << 8) | payload[3];

      latest_sensor_data.temperature = temp_raw / 100.0f;
      latest_sensor_data.humidity = hum_raw / 100.0f;
      latest_sensor_data.valid = true;

      LOG_INF("Sensor: Temp=%.2f C, Hum=%.2f %%",
              (double)latest_sensor_data.temperature,
              (double)latest_sensor_data.humidity);
    } else {
      LOG_WRN("Unexpected SENSOR payload length: %d", length);
    }
    break;

  case UART_PKT_ACK:
    if (length >= 1) {
      ota_service_handle_ack((uint8_t *)payload, length);
    }
    break;

  case UART_PKT_CMD:
    if (length >= 1) {
      uint8_t cmd = payload[0];
      const uint8_t *subpayload = &payload[1];
      uint8_t sublen = length - 1;

      LOG_INF("CMD received: 0x%02X (%d bytes)", cmd, sublen);

      switch (cmd) {
      case OTA_CMD_INIT:
        ota_service_start();
        break;
      case OTA_CMD_DATA:
        ota_service_receive_chunk(subpayload, sublen);
        break;
      case OTA_CMD_END:
        ota_service_finalize();
        break;
      case OTA_CMD_REBOOT:
        LOG_WRN("OTA_CMD_REBOOT received (not implemented)");
        break;
      default:
        LOG_WRN("Unknown CMD: 0x%02X", cmd);
        break;
      }
    } else {
      LOG_WRN("CMD packet too short");
    }
    break;

  default:
    LOG_INF("Unknown UART packet type: 0x%02X", type);
    break;
  }
}

// UART interrupt callback
static void uart_cb(const struct device *dev, void *user_data) {
  uint8_t c;

  while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {

    if (uart_irq_rx_ready(dev)) {
      int recv = uart_fifo_read(dev, &c, 1);
      if (recv > 0 && rx_len < BUF_SIZE) {
        rx_buf[rx_len++] = c;

        // Skip junk until valid preamble
        while (rx_len > 0 && rx_buf[0] != PREAMBLE) {
          memmove(rx_buf, rx_buf + 1, --rx_len);
        }

        // Wait for complete frame
        if (rx_len >= 4) {
          uint8_t length = rx_buf[2];
          size_t expected_len = 4 + length; // includes CRC

          if (rx_len >= expected_len) {
            print_hex_buffer(rx_buf, expected_len);
            handle_packet_raw(rx_buf, expected_len);
            memmove(rx_buf, rx_buf + expected_len, rx_len - expected_len);
            rx_len -= expected_len;
          }
        }
      }
    }

    if (uart_irq_tx_ready(dev)) {
      uart_tx_busy = false;
      uart_irq_tx_disable(dev);
      LOG_DBG("UART TX complete");
    }
  }
}

// UART driver initialization
int uart_comm_init(void) {
  if (!device_is_ready(uart_dev)) {
    LOG_ERR("UART device not ready");
    return -ENODEV;
  }

  uart_irq_callback_user_data_set(uart_dev, uart_cb, NULL);
  uart_irq_rx_enable(uart_dev);

  LOG_INF("UART initialized");
  return 0;
}

// Return UART device handle
const struct device *uart_comm_get_dev(void) { return uart_dev; }
