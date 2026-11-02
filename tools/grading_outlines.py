"""Generates the outline textures used by the grading preview (src/TerrainPreview.cpp).

Each texture outlines a terrain cell on the sides given by its instance ID, so that a region of cells gets a single outline:
  bits 0-3: an edge on the x-min, x-max, z-min and z-max side of the cell
  bits 4-7: a corner piece in the (x-min, z-min), (x-min, z-max), (x-max, z-min) and (x-max, z-max) corner,
            which connects the outline at concave corners of the region

Terrain selections map texture columns to x and rows to z (without rotation), so the x-min side is the left column of the image.
The textures are white, so the selection color is used as is. The line has the same profile as the game's tile outline 0x144161da.

Usage: python grading_outlines.py <output.dat> [preview directory]
"""
import struct
import sys
import time
import zlib

FSH_TYPE = 0x7ab50e44
GROUP = 0x4e414d47
SIZE = 64
PROFILE = [0, 136, 255, 85, 34, 17]  # alpha by distance from the cell edge in pixels
LINE_OFFSET = 2  # distance of the center of the line from the cell edge

X_MIN, X_MAX, Z_MIN, Z_MAX = 1, 2, 4, 8
CORNERS = [(X_MIN, Z_MIN), (X_MIN, Z_MAX), (X_MAX, Z_MIN), (X_MAX, Z_MAX)]


def profile(distance):
    return PROFILE[distance] if distance < len(PROFILE) else 0


def coverage(distance):
    # how far a line continues along its length past a perpendicular line: fully up to its center, then fading
    return 255 if distance <= LINE_OFFSET else profile(distance)


def masks():
    for edges in range(16):
        free_corners = [i for i, (x_side, z_side) in enumerate(CORNERS) if not edges & (x_side | z_side)]
        for subset in range(1 << len(free_corners)):
            corners = sum(1 << corner for bit, corner in enumerate(free_corners) if subset >> bit & 1)
            yield edges | corners << 4


def alpha(mask, row, column):
    distances = {X_MIN: column, X_MAX: SIZE - 1 - column, Z_MIN: row, Z_MAX: SIZE - 1 - row}
    result = 0
    for side, distance in distances.items():
        if mask & side:
            # stop at perpendicular edges, so that the line does not stick out at convex corners
            along = [distances[other] for other in ((Z_MIN, Z_MAX) if side in (X_MIN, X_MAX) else (X_MIN, X_MAX)) if mask & other]
            if all(d >= LINE_OFFSET for d in along):
                result = max(result, profile(distance))
    for i, (x_side, z_side) in enumerate(CORNERS):
        if mask >> 4 & 1 << i:
            dx, dz = distances[x_side], distances[z_side]
            result = max(result, profile(dx) * coverage(dz) // 255, profile(dz) * coverage(dx) // 255)
    return result


def dxt3(alphas):
    blocks = bytearray()
    for block_row in range(0, SIZE, 4):
        for block_column in range(0, SIZE, 4):
            bits = 0
            for i in range(16):
                a = alphas[(block_row + i // 4) * SIZE + block_column + i % 4]
                bits |= (a * 15 + 127) // 255 << (4 * i)
            blocks += struct.pack("<QHHI", bits, 0xffff, 0xffff, 0)  # white, color index 0
    return bytes(blocks)


def fsh(pixels):
    entry = struct.pack("<IHH4H", 0x61, SIZE, SIZE, 0, 0, 0, 0) + pixels + bytes(16)
    header = b"SHPI" + struct.pack("<II", 0x20 + len(entry), 1) + b"G264" + b"0000" + struct.pack("<I", 0x20) + b"Buy ERTS"
    return header + entry


def dbpf(entries):
    data = bytearray(96)
    index = bytearray()
    for (type_id, group, instance), payload in entries:
        index += struct.pack("<5I", type_id, group, instance, len(data), len(payload))
        data += payload
    index_offset = len(data)
    data += index
    now = int(time.time())
    header = b"DBPF" + struct.pack("<9I", 1, 0, 0, 0, 0, now, now, 7, len(entries)) + struct.pack("<5I", index_offset, len(index), 0, 0, 0)
    data[:len(header)] = header
    return bytes(data)


def png(path, alphas):
    rows = b"".join(b"\x00" + bytes(v for a in alphas[y * SIZE:(y + 1) * SIZE] for v in (255, 255, 255, a)) for y in range(SIZE))
    chunk = lambda kind, body: struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def main():
    entries = []
    for mask in sorted(set(masks())):
        alphas = [alpha(mask, row, column) for row in range(SIZE) for column in range(SIZE)]
        entries.append(((FSH_TYPE, GROUP, mask), fsh(dxt3(alphas))))
        if len(sys.argv) > 2:
            png(f"{sys.argv[2]}/outline_{mask:02x}.png", alphas)
    with open(sys.argv[1], "wb") as f:
        f.write(dbpf(entries))
    print(f"wrote {len(entries)} textures to {sys.argv[1]}")


if __name__ == "__main__":
    main()
