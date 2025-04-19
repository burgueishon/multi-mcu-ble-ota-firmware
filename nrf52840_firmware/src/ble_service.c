#include "ble_service.h"
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_service, LOG_LEVEL_INF);

static struct bt_conn *current_conn = NULL;

// 128-bit UUIDs for the service and characteristic
static struct bt_uuid_128 service_uuid =
    BT_UUID_INIT_128(0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34,
                     0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0);

static struct bt_uuid_128 sensor_char_uuid =
    BT_UUID_INIT_128(0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd,
                     0xef, 0x01, 0x23, 0x45, 0x67, 0x89);

static uint8_t sensor_value[8];

// Handle read requests from clients
static ssize_t read_sensor(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr, void *buf,
                           uint16_t len, uint16_t offset) {
  return bt_gatt_attr_read(conn, attr, buf, len, offset, sensor_value,
                           sizeof(sensor_value));
}

// Called when notifications are enabled/disabled
static void sensor_ccc_cfg_changed(const struct bt_gatt_attr *attr,
                                   uint16_t value) {
  bool notify_enabled = (value == BT_GATT_CCC_NOTIFY);
  LOG_INF("Notifications %s", notify_enabled ? "enabled" : "disabled");
}

// GATT service definition
BT_GATT_SERVICE_DEFINE(sensor_svc, BT_GATT_PRIMARY_SERVICE(&service_uuid),
                       BT_GATT_CHARACTERISTIC(&sensor_char_uuid.uuid,
                                              BT_GATT_CHRC_READ |
                                                  BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ, read_sensor,
                                              NULL, sensor_value),
                       BT_GATT_CCC(sensor_ccc_cfg_changed,
                                   BT_GATT_PERM_READ | BT_GATT_PERM_WRITE), );

// Connection callbacks
static void connected(struct bt_conn *conn, uint8_t err) {
  if (err == 0) {
    current_conn = bt_conn_ref(conn);
    LOG_INF("BLE client connected");
  } else {
    LOG_WRN("Connection failed (err %u)", err);
  }
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
  LOG_INF("BLE client disconnected (reason %u)", reason);
  if (current_conn) {
    bt_conn_unref(current_conn);
    current_conn = NULL;
  }
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};

void ble_service_init(void) {
  int err;

  err = bt_set_name("XiaoBLE");
  if (err) {
    LOG_ERR("Failed to set BLE name (err %d)", err);
  }

  err = bt_enable(NULL);
  if (err) {
    LOG_ERR("Bluetooth init failed (err %d)", err);
    return;
  }

  bt_conn_cb_register(&conn_callbacks);
  LOG_INF("Bluetooth initialized as XiaoBLE");

  static const uint8_t flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
  static const uint8_t svc_uuid[] = {0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56,
                                     0x34, 0x12, 0xf0, 0xde, 0xbc, 0x9a,
                                     0x78, 0x56, 0x34, 0x12};

  static const struct bt_data ad[] = {
      {.type = BT_DATA_FLAGS, .data_len = sizeof(flags), .data = &flags},
      {.type = BT_DATA_UUID128_ALL,
       .data_len = sizeof(svc_uuid),
       .data = svc_uuid}};

// Suppress warnings for deprecated API usage (still needed for BLE advertising in Zephyr 4.1+)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
// BLE advertising parameters (extended advertising not used here)
  static const struct bt_le_adv_param adv_params = {
      .id = BT_ID_DEFAULT,
      .sid = 0,
      .secondary_max_skip = 0,
      .options = BT_LE_ADV_OPT_USE_NAME | BT_LE_ADV_OPT_CONNECTABLE,
      .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
      .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
      .peer = NULL,
  };
#pragma GCC diagnostic pop

  err = bt_le_adv_start(&adv_params, ad, ARRAY_SIZE(ad), NULL, 0);
  if (err) {
    LOG_ERR("Advertising failed to start (err %d)", err);
  } else {
    LOG_INF("Advertising started with name and service UUID");
  }
}

void ble_service_send_sensor_data(const sensor_data_t *data) {
  memcpy(&sensor_value[0], &data->temperature, sizeof(float));
  memcpy(&sensor_value[4], &data->humidity, sizeof(float));

  if (current_conn && bt_gatt_is_subscribed(current_conn, &sensor_svc.attrs[1],
                                            BT_GATT_CCC_NOTIFY)) {
    bt_gatt_notify(current_conn, &sensor_svc.attrs[1], sensor_value,
                   sizeof(sensor_value));
    LOG_INF("Sensor data notified: Temp=%.2f C, Hum=%.2f %%",
            (double)data->temperature, (double)data->humidity);
  } else {
    LOG_DBG("No client subscribed. Notification skipped.");
  }
}
