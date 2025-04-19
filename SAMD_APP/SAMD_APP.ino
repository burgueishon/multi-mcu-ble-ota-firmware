#include <Arduino.h>
#include <SensirionI2cSht4x.h>
#include <Wire.h>
#include <SD.h>
#include <Arduino_CRC32.h>
#include <CRC32.h>
#include <SDU.h>

// UART protocol definitions
#define PREAMBLE 0xAA
#define UART_PKT_SENSOR 0x01
#define UART_PKT_CMD    0x02
#define UART_PKT_ACK    0x03

// OTA commands
#define OTA_CMD_INIT    0x00
#define OTA_CMD_DATA    0x01
#define OTA_CMD_END     0x02
#define OTA_CMD_REBOOT  0x03

#define UART_MAX_PAYLOAD_SIZE 64
#define UART_RX_BUF_SIZE 128
#define SENSOR_INTERVAL_MS 5000

#define LED_PIN LED_BUILTIN

// UART packet structure
struct UartPacket {
  uint8_t preamble;
  uint8_t type;
  uint8_t length;
  uint8_t payload[UART_MAX_PAYLOAD_SIZE];
  uint8_t crc;
} __attribute__((packed));

// UART reception buffer
uint8_t uart_rx_buf[UART_RX_BUF_SIZE];
size_t uart_rx_index = 0;

// OTA state
File otaFile;
const char *firmware_filename = "firmware.bin";
bool ota_in_progress = false;
uint32_t ota_blocks_received = 0;
unsigned long ota_last_activity_ms = 0;
const unsigned long OTA_TIMEOUT_MS = 10000;

SensirionI2cSht4x sht4x;

// CRC8 calculation
uint8_t crc8_ccitt(const uint8_t *data, size_t len, uint8_t seed = 0xFF) {
  uint8_t crc = seed;
  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; i++) {
      crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
    }
  }
  return crc;
}

// CRC32 calculation from file contents
uint32_t calculate_crc32_from_file(const char *filename) {
  File f = SD.open(filename);
  if (!f) return 0;

  CRC32 crc;
  uint8_t buf[64];

  while (f.available()) {
    size_t len = f.read(buf, sizeof(buf));
    crc.update(buf, len);
  }

  f.close();
  return crc.finalize();
}

// Send UART packet
void send_packet(uint8_t type, const uint8_t *payload, uint8_t length) {
  if (length > UART_MAX_PAYLOAD_SIZE) return;

  uint8_t packet[3 + length + 1];
  packet[0] = PREAMBLE;
  packet[1] = type;
  packet[2] = length;
  memcpy(&packet[3], payload, length);
  packet[3 + length] = crc8_ccitt(packet, 3 + length, 0xFF);

  Serial1.write(packet, 4 + length);
  Serial.print("UART TX: ");
  for (int i = 0; i < 4 + length; i++) {
    if (packet[i] < 0x10) Serial.print("0");
    Serial.print(packet[i], HEX); Serial.print(" ");
  }
  Serial.println();
}

// Handle incoming UART packet
void handle_packet(UartPacket *pkt) {
  if (pkt->preamble != PREAMBLE) return;

  Serial.println("Raw packet received:");
  for (int i = 0; i < 3 + pkt->length; i++) {
    if (((uint8_t *)pkt)[i] < 0x10) Serial.print("0");
    Serial.print(((uint8_t *)pkt)[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  Serial.print("CRC received: 0x");
  Serial.println(pkt->crc, HEX);
  Serial.print("Packet type: 0x");
  Serial.println(pkt->type, HEX);

  switch (pkt->type) {

    case UART_PKT_CMD: {
      if (pkt->length < 1) return;

      uint8_t cmd = pkt->payload[0];
      Serial.print("Command received: 0x");
      Serial.println(cmd, HEX);

      switch (cmd) {
        case OTA_CMD_INIT:
          Serial.println("OTA started");
          ota_in_progress = true;
          ota_last_activity_ms = millis();

          if (SD.exists(firmware_filename)) {
            SD.remove(firmware_filename);
            Serial.println("Previous firmware.bin deleted");
          }

          otaFile = SD.open(firmware_filename, FILE_WRITE);
          if (otaFile) {
            ota_blocks_received = 0;
            Serial.println("OTA file ready");
          } else {
            Serial.println("Error creating firmware.bin");
            ota_in_progress = false;
          }

          {
            uint8_t ack[] = { OTA_CMD_INIT };
            send_packet(UART_PKT_ACK, ack, sizeof(ack));
            Serial.println("ACK for INIT sent");
          }
          break;

        case OTA_CMD_DATA:
          if (!ota_in_progress) {
            Serial.println("OTA_CMD_DATA received out of sequence");
            return;
          }
          ota_last_activity_ms = millis();

          {
            size_t data_len = pkt->length - 1;
            uint8_t *data = &pkt->payload[1];

            Serial.print("Writing OTA chunk: ");
            for (size_t i = 0; i < data_len; i++) {
              if (data[i] < 0x10) Serial.print("0");
              Serial.print(data[i], HEX);
              Serial.print(" ");
            }
            Serial.println();

            if (otaFile) {
              otaFile.write(data, data_len);
              otaFile.flush();
              ota_blocks_received++;

              Serial.print("Block ");
              Serial.print(ota_blocks_received);
              Serial.print(" received (");
              Serial.print(data_len);
              Serial.println(" bytes)");

              uint8_t ack_payload[] = { OTA_CMD_DATA };
              send_packet(UART_PKT_ACK, ack_payload, sizeof(ack_payload));
            } else {
              Serial.println("File not available for writing");
            }
          }
          break;

        case OTA_CMD_END: {
          if (!ota_in_progress) {
            Serial.println("OTA_CMD_END received out of sequence");
            return;
          }
          ota_last_activity_ms = millis();  

          Serial.println("OTA_CMD_END received, closing file and calculating CRC32...");

          otaFile.flush();
          otaFile.close();
          ota_in_progress = false;

          File check = SD.open(firmware_filename);
          if (check) {
            Serial.print("Final firmware.bin size: ");
            Serial.println(check.size());
            check.close();
          } else {
            Serial.println("Could not open firmware.bin to check size");
          }

          uint32_t crc32 = calculate_crc32_from_file(firmware_filename);
          Serial.print("Calculated CRC32: 0x");
          Serial.println(crc32, HEX);

          uint8_t ack_payload[5];
          ack_payload[0] = OTA_CMD_END;
          ack_payload[1] = (crc32 >> 24) & 0xFF;
          ack_payload[2] = (crc32 >> 16) & 0xFF;
          ack_payload[3] = (crc32 >> 8) & 0xFF;
          ack_payload[4] = crc32 & 0xFF;
          send_packet(UART_PKT_ACK, ack_payload, sizeof(ack_payload));

          break;
        }

        case OTA_CMD_REBOOT: {
          Serial.println("Reboot command received");

          uint8_t ack_payload[] = { OTA_CMD_REBOOT };
          send_packet(UART_PKT_ACK, ack_payload, sizeof(ack_payload));
          Serial.println("ACK for REBOOT sent");

          delay(100);

          bool ok = rename_file(firmware_filename, "UPDATE.BIN");
          if (ok) {
            Serial.println("firmware.bin renamed to UPDATE.BIN");
            if (SD.exists("UPDATE.BIN")) {
              Serial.println("UPDATE.BIN found on SD");
            } else {
              Serial.println("UPDATE.BIN not found after renaming");
            }
          } else {
            Serial.println("Error renaming firmware.bin to UPDATE.BIN");
          }

          Serial.println("Rebooting SAMD21...");
          NVIC_SystemReset();
          break;
        }

        default:
          Serial.println("Unrecognized command in CMD packet");
          break;
      }

      break;
    }

    case UART_PKT_ACK:
      Serial.println("ACK received (SAMD21)");
      break;

    default:
      Serial.print("Unknown UART packet type: 0x");
      Serial.println(pkt->type, HEX);
      break;
  }
}

// Parse UART input buffer
void process_uart_rx() {
  static uint8_t buf[3 + UART_MAX_PAYLOAD_SIZE + 1];
  static int idx = 0, expected = 0;

  while (Serial1.available()) {
    uint8_t byte = Serial1.read();
    if (idx == 0 && byte != PREAMBLE) continue;

    buf[idx++] = byte;

    if (idx == 3) {
      expected = 3 + buf[2] + 1;
      if (expected > sizeof(buf)) {
        Serial.println("Invalid size, discarding packet");
        idx = 0;
        continue;
      }
    }

    if (idx == expected) {
      uint8_t crc_recv = buf[expected - 1];
      uint8_t crc_calc = crc8_ccitt(buf, expected - 1, 0xFF);

      Serial.print("CRC received: 0x");
      Serial.println(crc_recv, HEX);
      Serial.print("CRC expected: 0x");
      Serial.println(crc_calc, HEX);

      if (crc_recv == crc_calc) {
        UartPacket pkt;
        pkt.preamble = buf[0];
        pkt.type     = buf[1];
        pkt.length   = buf[2];
        memcpy(pkt.payload, &buf[3], pkt.length);
        pkt.crc      = crc_recv;

        Serial.print("Packet type: 0x");
        Serial.println(pkt.type, HEX);

        handle_packet(&pkt);
      } else {
        Serial.println("Invalid CRC. Packet discarded.");
      }

      idx = 0;
    }
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial1.begin(115200);
  delay(100);
  Serial.println("MKR Zero UART ready");

  Wire.begin();
  sht4x.begin(Wire, 0x44);
  Serial.println("SHT4x initialized");

  // Dummy read to initialize the sensor bus
  float t_dummy, h_dummy;
  sht4x.measureHighPrecision(t_dummy, h_dummy);

  if (!SD.begin()) {
    Serial.println("Error initializing SD card");
  } else {
    Serial.println("SD card detected");
  }

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

}

void loop() {
  process_uart_rx();

  if (ota_in_progress) {
    if (millis() - ota_last_activity_ms > OTA_TIMEOUT_MS) {
      Serial.println("OTA timeout. Reactivating sensors.");
      ota_in_progress = false;
    }
    return;
  }

  float temperature, humidity;
  if (sht4x.measureHighPrecision(temperature, humidity) == 0) {
    int16_t temp_int = temperature * 100;
    int16_t hum_int  = humidity * 100;

    uint8_t payload[4] = {
      (uint8_t)(temp_int >> 8), (uint8_t)(temp_int & 0xFF),
      (uint8_t)(hum_int >> 8), (uint8_t)(hum_int & 0xFF)
    };

    Serial.print("Temperature: ");
    Serial.print(temperature);
    Serial.print(" °C, Humidity: ");
    Serial.print(humidity);
    Serial.println(" %");

    send_packet(UART_PKT_SENSOR, payload, sizeof(payload));
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  } else {
    Serial.println("Error reading SHT4x sensor");
  }

  delay(SENSOR_INTERVAL_MS);
}

// Rename file on SD card
bool rename_file(const char* oldName, const char* newName) {
  Serial.println("rename_file() started");

  if (!SD.exists(oldName)) {
    Serial.println("Original file not found");
    return false;
  }
  Serial.println("Original file found");

  if (SD.exists(newName)) {
    SD.remove(newName);
    Serial.println("Previous UPDATE.BIN removed");
  } else {
    Serial.println("No previous UPDATE.BIN");
  }

  File oldFile = SD.open(oldName, FILE_READ);
  if (!oldFile) {
    Serial.println("Failed to open original file");
    return false;
  }
  Serial.println("Original file opened");

  File newFile = SD.open(newName, FILE_WRITE);
  if (!newFile) {
    Serial.println("Failed to create new file");
    oldFile.close();
    return false;
  }
  Serial.println("New file created");

  const size_t BUF_SZ = 64;
  uint8_t buf[BUF_SZ];
  size_t total = 0;

  Serial.println("Copying file content...");
  while (oldFile.available()) {
    size_t n = oldFile.read(buf, BUF_SZ);
    newFile.write(buf, n);
    total += n;
  }
  Serial.print("Copied ");
  Serial.print(total);
  Serial.println(" bytes");

  oldFile.close();
  newFile.close();

  if (SD.remove(oldName)) {
    Serial.println("Original file deleted");
  } else {
    Serial.println("Failed to delete original file");
  }

  return true;
}
