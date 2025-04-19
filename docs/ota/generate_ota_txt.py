# generate_ota_txt.py

def bin_to_hex_chunks(file_path, chunk_size=19):  # 19 bytes
    with open(file_path, "rb") as f:
        binary = f.read()
    chunks = [binary[i:i+chunk_size] for i in range(0, len(binary), chunk_size)]
    return chunks

def save_chunks_to_txt(chunks, txt_path):
    with open(txt_path, "w") as f:
        for chunk in chunks:
            hex_str = chunk.hex().upper()
            f.write(f"{hex_str}\n")

if __name__ == "__main__":
    bin_file = "SAMD_NO_LED.ino.bin"
    txt_file = "ota_commands_NO_LED.txt"

    chunks = bin_to_hex_chunks(bin_file)
    save_chunks_to_txt(chunks, txt_file)

    print(f"✅ {txt_file} generated with {len(chunks)} chunks.")

