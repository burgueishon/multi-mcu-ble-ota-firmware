#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include <zephyr/bluetooth/uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

// Global OTA state
extern bool ota_active;
extern enum ota_phase_t ota_phase;

// OTA transmission phases
enum ota_phase_t {
    OTA_PHASE_IDLE = 0,             // Not doing OTA
    OTA_PHASE_WAIT_ALL_CHUNKS,     // Waiting for all BLE chunks (INIT received)
    OTA_PHASE_WAIT_ACK_INIT,       // INIT sent, waiting for ACK
    OTA_PHASE_SENDING_DATA,        // Sending data chunks to target
    OTA_PHASE_WAIT_ACK_END,        // END sent, waiting for CRC and ACK
    OTA_PHASE_DONE,                // OTA completed
    OTA_PHASE_ABORTED,             // OTA failed or was aborted
};

// BLE command markers used to indicate OTA start/end
#define BLE_CMD_INIT 0x10
#define BLE_CMD_END  0x11

// Called when a BLE chunk is received (from BLE write callback)
void ota_queue_push(const uint8_t *data, uint16_t len);

// Initializes OTA BLE service and work queue
void ota_service_init(void);

// Main loop processor (usually called from main loop or k_work)
void ota_process_loop(void);

// Returns true if OTA is currently active
bool ota_is_active(void);

// Handle ACK received from target device
void ota_service_handle_ack(uint8_t *payload, uint8_t length);

// Get current OTA phase
enum ota_phase_t ota_get_phase(void);

#ifdef __cplusplus
}
#endif

#endif // OTA_SERVICE_H
