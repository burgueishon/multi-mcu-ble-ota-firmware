import asyncio
from bleak import BleakClient, BleakScanner

SERVICE_UUID = "55555555-5555-4444-3333-222211111111"
CHAR_UUID = "aaaaaaaa-aaaa-9999-8888-777766666666"
DEVICE_NAME = "XiaoBLE"
FILENAME = "ota_commands_NO_LED.txt"

MAX_CHUNK_SIZE = 19  # max payload
BLE_CMD_DATA = 0x01
BLE_CMD_INIT = 0x10
BLE_CMD_END = 0x11

def load_chunks_from_txt(filename):
    chunks = []
    with open(filename, "r") as f:
        for line in f:
            hex_str = line.strip()
            if hex_str:
                data = bytes.fromhex(hex_str)
                if len(data) <= MAX_CHUNK_SIZE:
                    chunks.append(data)
                else:
                    print(f"Chunk too large ({len(data)} bytes), rejected.")
    return chunks

async def send_ota_commands():
    print("Scanning BLE devices...")
    devices = await BleakScanner.discover()
    target = next((d for d in devices if d.name == DEVICE_NAME), None)

    if not target:
        print(f" {DEVICE_NAME} device not found.")
        return

    print(f"Connected to {DEVICE_NAME} ({target.address})...")
    async with BleakClient(target) as client:
        print("Connected. Waiting befor starting transfer...")
        await asyncio.sleep(1)

        chunks = load_chunks_from_txt(FILENAME)
        total_chunks = len(chunks)

        if total_chunks == 0:
            print("not chunks found in .txt")
            return

        # send BLE_CMD_INIT (0x10)
        print("Sending BLE_CMD_INIT: {BLE_CMD_INIT:#04x}")
        await client.write_gatt_char(CHAR_UUID, bytes([BLE_CMD_INIT]), response=False)
        await asyncio.sleep(0.1)

        print("Sending OTA Chunks...")
        for i, chunk in enumerate(chunks, 1):
            data = bytes([BLE_CMD_DATA]) + chunk
            try:
                print(f"Sending chunk {i}/{total_chunks} ({len(data)} bytes)")
                await client.write_gatt_char(CHAR_UUID, data, response=False)
                await asyncio.sleep(0.05)  # 🕒 Control de flujo
            except Exception as e:
                print(f"Error sending chunk {i}: {e}")
                return

        # sending BLE_CMD_END (0x11)
        print("sending  BLE_CMD_END (0x11)...")
        try:
            await client.write_gatt_char(CHAR_UUID, bytes([BLE_CMD_END]), response=False)
            print(" BLE_CMD_END sent. OTA should start.")
        except Exception as e:
            print(f"Error sending BLE_CMD_END: {e}")

if __name__ == "__main__":
    asyncio.run(send_ota_commands())
