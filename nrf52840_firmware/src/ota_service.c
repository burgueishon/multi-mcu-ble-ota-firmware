// OTA service implementation for BLE-triggered firmware updates

#include "ota_service.h"
#include "uart_comm.h"
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(ota_service, LOG_LEVEL_DBG);

// Constants
#define OTA_MAX_CHUNK_SIZE 20 // Max BLE write chunk size
#define OTA_QUEUE_SIZE 3000   // Max number of OTA BLE chunks stored

// OTA state
static bool ack_received = false;
static bool ota_init_sent = false;
bool ota_active = false;
enum ota_phase_t ota_phase = OTA_PHASE_IDLE;

static uint8_t ota_retry_count = 0;
#define OTA_MAX_RETRIES 3
#define OTA_ACK_TIMEOUT_MS 4000 // Retry delay

static uint32_t ota_chunk_index = 0;
static uint32_t ble_chunk_counter = 0;
static uint8_t last_chunk[OTA_MAX_CHUNK_SIZE];
static uint16_t last_len;

// BLE UUIDs for OTA service
static struct bt_uuid_128 ota_service_uuid =
    BT_UUID_INIT_128(0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0x44, 0x44,
                     0x55, 0x55, 0x55, 0x55, 0x55, 0x55);

static struct bt_uuid_128 ota_data_char_uuid =
    BT_UUID_INIT_128(0x66, 0x66, 0x66, 0x66, 0x77, 0x77, 0x88, 0x88, 0x99, 0x99,
                     0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa);

// BLE work structure
static struct k_work ota_send_work;

// BLE chunk queue
struct ota_chunk {
  uint8_t data[OTA_MAX_CHUNK_SIZE];
  uint16_t len;
};

static struct ota_chunk ota_queue[OTA_QUEUE_SIZE];
static volatile int ota_queue_head = 0;
static volatile int ota_queue_tail = 0;

/* Forward declarations */
static void ota_send_work_fn(struct k_work *work);
static const char *ota_phase_str(enum ota_phase_t phase);

/* Retry timer handler */
static void ota_retry_handler(struct k_timer *timer) {
  if (ota_phase == OTA_PHASE_WAIT_ACK_INIT) {
    ota_retry_count++;
    if (ota_retry_count > OTA_MAX_RETRIES) {
      ota_phase = OTA_PHASE_ABORTED;
      ota_active = false;
      LOG_ERR("OTA aborted. No ACK for INIT");
    } else {
      LOG_WRN("Retrying OTA_CMD_INIT (%d/%d)", ota_retry_count,
              OTA_MAX_RETRIES);
      int ret = uart_comm_send_cmd(UART_PKT_CMD, OTA_CMD_INIT, NULL, 0);
      if (ret == 0) {
        LOG_INF("Resent OTA_CMD_INIT");
      } else {
        LOG_ERR("Failed to resend INIT: %d", ret);
      }
    }
  } else if (ota_phase == OTA_PHASE_SENDING_DATA) {
    ota_retry_count++;
    if (ota_retry_count > OTA_MAX_RETRIES) {
      ota_phase = OTA_PHASE_ABORTED;
      ota_active = false;
      LOG_ERR("OTA aborted. No ACK for DATA");
    } else {
      LOG_WRN("Retrying OTA_CMD_DATA (%d/%d)", ota_retry_count,
              OTA_MAX_RETRIES);
      ack_received = true; // allow resend in process loop
      k_work_submit(&ota_send_work);
    }
  }
}

K_TIMER_DEFINE(ota_timer, ota_retry_handler, NULL);

/* Work item to continue OTA logic */
static void ota_send_work_fn(struct k_work *work) { ota_process_loop(); }

/* Clear all OTA state */
void ota_service_reset(void) {
  ota_active = false;
  ota_init_sent = false;
  ota_retry_count = 0;
  ota_chunk_index = 0;
  ble_chunk_counter = 0;  
  ota_phase = OTA_PHASE_IDLE;
  k_timer_stop(&ota_timer);
  LOG_INF("OTA reset complete");
}

/* OTA state accessors */
void ota_start(void) { ota_active = true; }
bool ota_is_active(void) { return ota_active; }
void ota_set_active(bool active) { ota_active = active; }
enum ota_phase_t ota_get_phase(void) { return ota_phase; }

static bool ota_queue_is_empty(void) {
  return ota_queue_head == ota_queue_tail;
}

/* Push received BLE chunk into circular queue */
void ota_queue_push(const uint8_t *data, uint16_t len) {
  if (len > OTA_MAX_CHUNK_SIZE) {
    LOG_ERR("Chunk too large: %d", len);
    return;
  }

  // Handle BLE commands directly
  uint8_t ble_cmd = data[0];
  switch (ble_cmd) {
  case BLE_CMD_INIT:
    if (ota_phase != OTA_PHASE_IDLE && ota_phase != OTA_PHASE_DONE) {
      LOG_WRN("BLE_CMD_INIT unexpected in phase %s", ota_phase_str(ota_phase));
      return;
    }
    LOG_INF("BLE_CMD_INIT received. Waiting for BLE chunks...");
    ota_set_active(true);
    ota_phase = OTA_PHASE_WAIT_ALL_CHUNKS;
    return;

  case BLE_CMD_END:
    if (ota_phase != OTA_PHASE_WAIT_ALL_CHUNKS) {
      LOG_WRN("BLE_CMD_END unexpected in phase %s", ota_phase_str(ota_phase));
      return;
    }
    LOG_INF("BLE_CMD_END received. OTA will begin");
    ota_phase = OTA_PHASE_IDLE;
    return;

  default:
    break;
  }

  // Push raw chunk data to queue
  int next_head = (ota_queue_head + 1) % OTA_QUEUE_SIZE;
  if (next_head == ota_queue_tail) {
    LOG_WRN("OTA queue full. Discarded chunk: %02X %02X %02X %02X", data[0],
            data[1], data[2], data[3]);
    return;
  }

    memcpy(ota_queue[ota_queue_head].data, data + 1, len - 1);
    ota_queue[ota_queue_head].len = len - 1;

    ble_chunk_counter++;
    LOG_DBG("Received BLE chunk #%u (%u bytes)", ble_chunk_counter, len - 1);

    ota_queue_head = next_head;

}

static bool ota_queue_pop(uint8_t *out_data, uint16_t *out_len) {
  if (ota_queue_is_empty()) {
    return false;
  }

  memcpy(out_data, ota_queue[ota_queue_tail].data,
         ota_queue[ota_queue_tail].len);
  *out_len = ota_queue[ota_queue_tail].len;
  ota_queue_tail = (ota_queue_tail + 1) % OTA_QUEUE_SIZE;
  return true;
}

/* BLE write callback */
static ssize_t ota_data_write_cb(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset,
                                 uint8_t flags) {
  if (len > OTA_MAX_CHUNK_SIZE) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  }

  ota_queue_push((const uint8_t *)buf, len);
  ota_init_sent = false;
  return len;
}

/* BLE OTA service declaration */
BT_GATT_SERVICE_DEFINE(ota_svc, BT_GATT_PRIMARY_SERVICE(&ota_service_uuid),
                       BT_GATT_CHARACTERISTIC(&ota_data_char_uuid.uuid,
                                              BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                              BT_GATT_PERM_WRITE, NULL,
                                              ota_data_write_cb, NULL));

/* Initialize OTA service and work item */
void ota_service_init(void) {
  k_work_init(&ota_send_work, ota_send_work_fn);
  LOG_INF("OTA BLE service initialized");
}

/* Debug print phase name */
static const char *ota_phase_str(enum ota_phase_t phase) {
  switch (phase) {
  case OTA_PHASE_IDLE:
    return "IDLE";
  case OTA_PHASE_WAIT_ALL_CHUNKS:
    return "WAIT_ALL_CHUNKS";
  case OTA_PHASE_WAIT_ACK_INIT:
    return "WAIT_ACK_INIT";
  case OTA_PHASE_SENDING_DATA:
    return "SENDING_DATA";
  case OTA_PHASE_WAIT_ACK_END:
    return "WAIT_ACK_END";
  case OTA_PHASE_DONE:
    return "DONE";
  case OTA_PHASE_ABORTED:
    return "ABORTED";
  default:
    return "UNKNOWN";
  }
}

/**
 * @brief Handles an ACK packet received via UART.
 *
 * Validates the ACK command and advances the OTA flow according to the current
 * phase.
 *
 * @param payload  Pointer to the ACK packet payload (must contain at least 1
 * byte)
 * @param length   Length of the received payload
 */
void ota_service_handle_ack(uint8_t *payload, uint8_t length) {
  if (length < 1) {
    LOG_WRN("ACK received with invalid length (%d)", length);
    return;
  }

  uint8_t ack_cmd = payload[0];
  LOG_INF("ACK received in ota_service_handle_ack: cmd=0x%02X", ack_cmd);

  switch (ota_phase) {

  case OTA_PHASE_WAIT_ACK_INIT:
    if (ack_cmd == OTA_CMD_INIT) {
      LOG_INF("ACK for OTA_CMD_INIT received. Starting data transmission...");
      ota_retry_count = 0;
      ota_phase = OTA_PHASE_SENDING_DATA;
      k_timer_stop(&ota_timer);
      ack_received = true;
      k_work_submit(&ota_send_work);
    } else {
      LOG_WRN("Expected ACK for INIT, but got cmd=0x%02X", ack_cmd);
    }
    break;

  case OTA_PHASE_SENDING_DATA:
    if (ack_cmd == OTA_CMD_DATA) {
      LOG_INF("ACK for OTA_CMD_DATA received");
      ack_received = true;
      ota_retry_count = 0;
      k_timer_stop(&ota_timer);
      k_work_submit(&ota_send_work);
    } else {
      LOG_WRN("Expected ACK for DATA, but got cmd=0x%02X", ack_cmd);
    }
    break;

  case OTA_PHASE_WAIT_ACK_END:
    if (ack_cmd == OTA_CMD_END) {
      LOG_INF("ACK for OTA_CMD_END received");

      if (length == 5) {
        uint32_t crc = ((uint32_t)payload[1] << 24) |
                       ((uint32_t)payload[2] << 16) |
                       ((uint32_t)payload[3] << 8) | ((uint32_t)payload[4]);

        LOG_INF("CRC32 received: 0x%08X", crc);
        // Optional: validate CRC here
      } else {
        LOG_WRN("ACK for END received with unexpected length (len=%d)", length);
      }

      int ret = uart_comm_send_cmd(UART_PKT_CMD, OTA_CMD_REBOOT, NULL, 0);
      if (ret == 0) {
        LOG_INF("Sent OTA_CMD_REBOOT to SAMD21");
      } else {
        LOG_ERR("Failed to send OTA_CMD_REBOOT: %d", ret);
      }

      k_sleep(K_MSEC(50));
      ota_service_reset();
      ota_phase = OTA_PHASE_DONE;
      ota_active = false;
      k_timer_stop(&ota_timer);
    } else {
      LOG_WRN("Expected ACK for END, but got cmd=0x%02X", ack_cmd);
    }
    break;

  default:
    LOG_WRN("Unexpected ACK in phase: %d (cmd=0x%02X)", ota_phase, ack_cmd);
    break;
  }
}
/**
 * @brief Processes the current state of the OTA flow.
 *
 * This function is called periodically (e.g., from a thread or work item)
 * to advance the OTA state machine, handling INIT, data transmission,
 * and END depending on the current phase.
 */
void ota_process_loop(void) {
  if (!ota_active) {
    return;
  }

  static enum ota_phase_t last_phase_logged = -1;
  if (ota_phase != last_phase_logged) {
    LOG_DBG("OTA process loop: current phase = %s", ota_phase_str(ota_phase));
    last_phase_logged = ota_phase;
  }

  switch (ota_phase) {

  case OTA_PHASE_IDLE:
    LOG_INF("Starting OTA: sending OTA_CMD_INIT");
    ota_retry_count = 0;
    ota_init_sent = true;
    ota_phase = OTA_PHASE_WAIT_ACK_INIT;

    if (uart_comm_send_cmd(UART_PKT_CMD, OTA_CMD_INIT, NULL, 0) == 0) {
      LOG_INF("OTA_CMD_INIT sent");
      k_timer_start(&ota_timer, K_MSEC(OTA_ACK_TIMEOUT_MS),
                    K_MSEC(OTA_ACK_TIMEOUT_MS));
    } else {
      LOG_ERR("Failed to send OTA_CMD_INIT");
      ota_phase = OTA_PHASE_ABORTED;
      ota_active = false;
    }
    break;

  case OTA_PHASE_SENDING_DATA: {
    if (!ack_received) {
      break;
    }

    if (ota_retry_count == 0) {
      if (!ota_queue_pop(last_chunk, &last_len)) {
        LOG_INF("All chunks sent. Sending OTA_CMD_END...");
        int ret = uart_comm_send_cmd(UART_PKT_CMD, OTA_CMD_END, NULL, 0);
        if (ret == 0) {
          LOG_INF("OTA_CMD_END sent. Waiting for CRC32...");
          ota_phase = OTA_PHASE_WAIT_ACK_END;
          ota_retry_count = 0;
          k_timer_start(&ota_timer, K_MSEC(OTA_ACK_TIMEOUT_MS), K_NO_WAIT);
        } else {
          LOG_ERR("Failed to send OTA_CMD_END: %d", ret);
          ota_phase = OTA_PHASE_ABORTED;
          ota_active = false;
        }
        break;
      }
      ota_chunk_index++;
      LOG_DBG("Sending chunk #%u", ota_chunk_index);
    }

    int ret =
        uart_comm_send_cmd(UART_PKT_CMD, OTA_CMD_DATA, last_chunk, last_len);
    if (ret == 0) {
      LOG_INF("OTA_CMD_DATA sent (%u bytes)", last_len);
      ack_received = false;
      ota_retry_count = 0;
      k_timer_start(&ota_timer, K_MSEC(OTA_ACK_TIMEOUT_MS), K_NO_WAIT);
    } else {
      LOG_ERR("Failed to send OTA_CMD_DATA: %d", ret);
      ota_phase = OTA_PHASE_ABORTED;
      ota_active = false;
    }
    break;
  }

  case OTA_PHASE_WAIT_ACK_INIT:
  case OTA_PHASE_WAIT_ACK_END:
  case OTA_PHASE_WAIT_ALL_CHUNKS:
  case OTA_PHASE_DONE:
  case OTA_PHASE_ABORTED:
    break;

  default:
    LOG_WRN("Unknown OTA phase: %d", ota_phase);
    break;
  }
}
