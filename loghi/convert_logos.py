#!/usr/bin/env python3
# Converte i 4 loghi nuovi in .h RGB565 PROGMEM per xevious.ino
# Target: 220 x 64 (come gli altri loghi), centrato, pad nero

from PIL import Image
import os

SRC_DIR = r"C:\Users\Tecnico\Desktop\XEVIOUS PROJECT\Files\loghi"
DST_DIR = r"C:\Users\Tecnico\Desktop\XEVIOUS PROJECT\Files\xevious"

TARGET_W = 220
TARGET_H = 64

# (src filename, define prefix, output filename, array name)
JOBS = [
    ("gradius 2.png",    "GRADIUS2",    "logo_gradius2.h",    "logo_gradius2_data"),
    ("1942.jpeg",        "G1942",       "logo_1942.h",        "logo_1942_data"),
    ("1943.jpeg",        "G1943",       "logo_1943.h",        "logo_1943_data"),
    ("crisis force.jpeg","CRISISFORCE", "logo_crisisforce.h", "logo_crisisforce_data"),
]

def rgb_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def convert(src, prefix, out_name, array_name):
    src_path = os.path.join(SRC_DIR, src)
    out_path = os.path.join(DST_DIR, out_name)
    print(f"[+] {src}  ->  {out_name}")

    img = Image.open(src_path).convert("RGB")
    w, h = img.size

    # Ridimensiona mantenendo aspect ratio, altezza = 64
    new_w = int(w * TARGET_H / h)
    img = img.resize((new_w, TARGET_H), Image.LANCZOS)

    # Se troppo largo, ridimensiona a 220 (larghezza fissa)
    if new_w > TARGET_W:
        # Ridimensiona forzando 220 (riduce un po' aspect ratio se serve)
        img = img.resize((TARGET_W, TARGET_H), Image.LANCZOS)
        new_w = TARGET_W

    # Canvas nero 220x64 centrato
    canvas = Image.new("RGB", (TARGET_W, TARGET_H), (0, 0, 0))
    x_off = (TARGET_W - new_w) // 2
    canvas.paste(img, (x_off, 0))

    # Dump RGB565
    with open(out_path, "w") as f:
        f.write("#pragma once\n")
        f.write("#include <Arduino.h>\n\n")
        f.write(f"#define {prefix}_LOGO_W {TARGET_W}\n")
        f.write(f"#define {prefix}_LOGO_H {TARGET_H}\n\n")
        f.write(f"static const uint16_t {array_name}[] PROGMEM = {{\n")

        px = canvas.load()
        for y in range(TARGET_H):
            row = []
            for x in range(TARGET_W):
                r, g, b = px[x, y]
                row.append(f"0x{rgb_to_rgb565(r, g, b):04X}")
            # 16 valori per riga per leggibilita'
            for i in range(0, len(row), 16):
                chunk = row[i:i+16]
                f.write("  " + ",".join(chunk) + ",\n")
        f.write("};\n")

    print(f"    OK -> {out_path}")

for job in JOBS:
    convert(*job)

print("\nFatto!")
