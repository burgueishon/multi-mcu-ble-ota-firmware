#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "uart_comm.h"
#include "ble_service.h"
#include "ota_service.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

// Device Tree alias for the LED pin
#define LED_NODE DT_ALIAS(led0)

// OTA processing thread configuration
#define OTA_THREAD_STACK_SIZE 1024
#define OTA_THREAD_PRIORITY   5

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

/**
 * @brief Thread to handle OTA processing.
 *
 * Runs at high priority and processes OTA frames when active.
 */
static void ota_thread(void *unused1, void *unused2, void *unused3)
{
    while (1) {
        if (ota_is_active()) {
            ota_process_loop();
            k_msleep(1);   // Responsive during OTA
        } else {
            k_msleep(50);  // Idle when no OTA
        }
    }
}

// Define and start the OTA thread
K_THREAD_DEFINE(ota_thread_id,
                OTA_THREAD_STACK_SIZE,
                ota_thread,
                NULL, NULL, NULL,
                OTA_THREAD_PRIORITY,
                0, 0);

/**
 * @brief Application entry point
 *
 * Initializes peripherals, services, and enters main loop.
 */
int main(void)
{
    bool led_on = false;
    const sensor_data_t *sensor = NULL;

    LOG_INF("Booting application");

    // Initialize LED GPIO
    if (!device_is_ready(led.port)) {
        LOG_ERR("LED device not ready");
        // Halt if LED is unavailable
        while (1) {
            k_msleep(1000);
        }
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    // Initialize UART communication
    if (uart_comm_init() != 0) {
        LOG_ERR("UART initialization failed");
        // Halt on UART init failure
        while (1) {
            k_msleep(1000);
        }
    }

    // Initialize BLE and OTA services
    ble_service_init();
    ota_service_init();

    LOG_INF("Initialization complete");

    // Main loop: blink LED, read sensor, send data via BLE
    while (1) {
        // Toggle LED - board
        led_on = !led_on;
        gpio_pin_set_dt(&led, led_on);

        // Fetch latest sensor data
        sensor = uart_comm_get_sensor_data();
        if (sensor && sensor->valid) {
            LOG_INF("Sensor: %.2f°C, %.2f%% RH",
                    (double)sensor->temperature,
                    (double)sensor->humidity);
            ble_service_send_sensor_data(sensor);
        }

        k_msleep(500);
    }
}
