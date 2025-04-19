#ifndef BLE_SERVICE_H
#define BLE_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "uart_comm.h" // For sensor_data_t
#include <zephyr/types.h>

/**
 * @brief Initialize the BLE service and characteristics.
 */
void ble_service_init(void);

/**
 * @brief Send sensor data via BLE notification.
 *
 * @param data Pointer to valid sensor data structure.
 */
void ble_service_send_sensor_data(const sensor_data_t *data);

#ifdef __cplusplus
}
#endif

#endif // BLE_SERVICE_H
