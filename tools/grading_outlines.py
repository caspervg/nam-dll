"""Generates the outline textures used by the grading preview (src/TerrainPreview.cpp).

Each texture outlines a terrain cell on the sides given by its instance ID, so that a region of cells gets a single outline:
  bits 0-3: an edge on the x-min, x-max, z-min and z-max side of the cell
  bits 4-7: a corner piece in the (x-min, z-min), (x-min, z-max), (x-max, z-min) and (x-max, z-max) corner,
            which connects the outline at concave corners of the region

Terrain selections map texture columns to x and rows to z (without rotation), so the x-min side is the left column of the image.
The textures are white, so the selection color is used as is. The line has the same profile as the game's tile outline 0x144161da.

The same package holds the wireframe of the graded terrain: two white models drawn as effects, which the DLL places,
colors and fades per line.
  nam_grading_line: a line along x from 0 to 16 m, one terrain cell, which is rotated and stretched onto a cell edge
  nam_grading_dash: a short vertical dash, stacked into dotted lines between the actual and the graded terrain
The effects are copies of a NAM puzzle piece preview effect, without pinning the particles to the terrain.

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


def fsh(pixels, size=SIZE):
    entry = struct.pack("<IHH4H", 0x61, size, size, 0, 0, 0, 0) + pixels + bytes(16)
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


S3D_TYPE = 0x5ad0e817
S3D_GROUP = 0xbadb57f1
MODEL_TEXTURE_GROUP = 0x1abe787d
EFFDIR_TYPE = 0xea5118b0
EFFDIR_GROUP = 0xea5118b1
WIREFRAME_TEXTURE = 0x4e474c00
LINE_MODEL = 0x4e474c01
DASH_MODEL = 0x4e474c02
WIREFRAME_EFFECTS = 0x4e474c00
LINE_WIDTH = 0.4  # meters, about 2 pixels at the closest zoom
DASH_WIDTH = 0.3
DASH_LENGTH = 0.8  # the DLL stacks them every 1.6 m

# EFFDIR of the NAM preview effect preview_diagonal_bridge_009 and its _red variant: one model particle each,
# re-emitted every 0.1 s with a lifetime of 0.1 s, pinned to the terrain.
EFFECTS_TEMPLATE = bytes.fromhex(
    "0400020002000000024000000000000001040000cdcccc3dcdcccc3dcdcccc3d00000000cdcccc3d000080bf000080bf000080bf000080bf00000000"
    "0000803f00000000000000000000803f0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000010000000000803f010000000000803f0000803f0000803f010000000000003f01000000"
    "00004842010000000000803f010000000000803f05180f5f030000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000803f00000000000000000000803f00000000000000000000000000000000000000000000"
    "000000000000286b6ece000000000000803f00401cc600401c4600000000000001000000803f0000a0400000a0400000484200004842cdcccc3dcdcc"
    "4c3e000000000000000000000000000000000000000000000000000000000000803f0000000000000000000000000000000000000000000000000000"
    "00000000000000008040010000000000c84300008040024000000000000001040000cdcccc3dcdcccc3dcdcccc3d00000000cdcccc3d000080bf0000"
    "80bf000080bf000080bf000000000000803f00000000000000000000803f000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000010000000000803f010000000000803fcdcc4c3ecdcc"
    "4c3e010000000000003f0100000000004842010000000000803f010000000000803f05180f5f03000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000803f00000000000000000000803f0000000000000000"
    "0000000000000000000000000000000000000000286b6ece000000000000803f00401cc600401c4600000000000001000000803f0000a0400000a040"
    "0000484200004842cdcccc3dcdcc4c3e000000000000000000000000000000000000000000000000000000000000803f000000000000000000000000"
    "000000000000000000000000000000000000000000008040010000000000c84300008040010000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000100000000000200020000000000000001000000010000001b000000707265766965775f646961676f6e"
    "616c5f6272696467655f30303900000000000000803f0000000000000000000000000000803f0000000000000000000000000000803f000000000000"
    "0000000000000000803f000000000106010010000000803f0000803f0000803f0000803f0000000000000000000000000000000000000000cccccccc"
    "cccccccc0000000001000000010000001f000000707265766965775f646961676f6e616c5f6272696467655f3030395f72656400000000000000803f"
    "0000000000000000000000000000803f0000000000000000000000000000803f0000000000000000000000000000803f000000000106010010000000"
    "803f0000803f0000803f0000803f0000000001000000000000000000000000000000cccccccccccccccc1b000000707265766965775f646961676f6e"
    "616c5f6272696467655f303039000000001f000000707265766965775f646961676f6e616c5f6272696467655f3030395f7265640100000003000000"
    "656e64ffffffffcd00000500000000401c4500409c4400401c4400009c4300001c430000000000000000000080400000e04000000000000000000000")
PARTICLE_OFFSETS = [8, 442]  # the two particle descriptions, see effdir.md in sc4-effects-extensions
PIN_TO_TERRAIN = 1 << 14  # in the leading bitset at +0x0
COLOR = 0x90  # color curve: count, then RGB
ALPHA = 0xa0  # alpha curve: count, then alpha
MODEL_KEY = 0xc0


def wireframe_effects():
    data = bytearray(EFFECTS_TEMPLATE)
    for start, model in zip(PARTICLE_OFFSETS, (LINE_MODEL, DASH_MODEL)):
        flags, = struct.unpack_from("<I", data, start)
        assert flags & PIN_TO_TERRAIN and struct.unpack_from("<I", data, start + MODEL_KEY)[0] == 0x5f0f1805
        struct.pack_into("<I", data, start, flags & ~PIN_TO_TERRAIN)
        struct.pack_into("<I3f", data, start + COLOR, 1, 1.0, 1.0, 1.0)  # colored per line by the DLL
        struct.pack_into("<If", data, start + ALPHA, 1, 1.0)  # faded per line by the DLL
        struct.pack_into("<I", data, start + MODEL_KEY, model)
    name = lambda text: struct.pack("<I", len(text)) + text
    for old, new in ((b"preview_diagonal_bridge_009_red", b"nam_grading_dash"), (b"preview_diagonal_bridge_009", b"nam_grading_line")):
        assert data.count(name(old)) == 2
        data = data.replace(name(old), name(new))
    return bytes(data)


def s3d(quads, name):
    """Minimal S3D with one textured, double-sided mesh of (x, y, z, u, v) quads."""
    block = lambda tag, body: tag + struct.pack("<I", 8 + len(body)) + body
    vertices = [vertex for quad in quads for vertex in quad]
    indices = [4 * q + i for q in range(len(quads)) for i in (0, 1, 2, 0, 2, 3)]
    material = (struct.pack("<IBBBBHIBB", 0x23, 4, 3, 1, 0, 0x7fff, 0, 0, 1)  # alpha test, depth test, texturing
                + struct.pack("<IBBBBHHB", WIREFRAME_TEXTURE, 2, 2, 1, 1, 0, 0, len(name) + 1) + name + b"\0")
    body = (block(b"HEAD", struct.pack("<HH", 1, 5))
            + block(b"VERT", struct.pack("<IHHI", 1, 0, len(vertices), 0x80004001) + b"".join(struct.pack("<5f", *v) for v in vertices))
            + block(b"INDX", struct.pack("<IHHH", 1, 0, 2, len(indices)) + struct.pack(f"<{len(indices)}H", *indices))
            + block(b"PRIM", struct.pack("<IHIII", 1, 1, 0, 0, len(indices)))
            + block(b"MATS", struct.pack("<I", 1) + material)
            + block(b"ANIM", struct.pack("<HHHIfH", 1, 0, 3, 0, 0.0, 1) + struct.pack("<BB", len(name) + 1, 0) + name + b"\0" + struct.pack("<4H", 0, 0, 0, 0))
            + block(b"PROP", struct.pack("<I", 0))
            + block(b"REGP", struct.pack("<I", 0)))
    return b"3DMD" + struct.pack("<I", 8 + len(body)) + body


def ribbons(length, width, axis):
    """Two crossed ribbons along an axis, so that the line has the same width from every camera angle."""
    h = width / 2
    if axis == "x":
        return [[(0, 0, -h, 0, 0), (length, 0, -h, 1, 0), (length, 0, h, 1, 1), (0, 0, h, 0, 1)],
                [(0, -h, 0, 0, 0), (length, -h, 0, 1, 0), (length, h, 0, 1, 1), (0, h, 0, 0, 1)]]
    return [[(-h, 0, 0, 0, 0), (-h, length, 0, 1, 0), (h, length, 0, 1, 1), (h, 0, 0, 0, 1)],
            [(0, 0, -h, 0, 0), (0, length, -h, 1, 0), (0, length, h, 1, 1), (0, 0, h, 0, 1)]]


def wireframe_entries():
    white = bytes(struct.pack("<QHHI", (1 << 64) - 1, 0xffff, 0xffff, 0) * 4)  # 8x8 DXT3, opaque white
    return [
        ((FSH_TYPE, MODEL_TEXTURE_GROUP, WIREFRAME_TEXTURE), fsh(white, 8)),
        ((S3D_TYPE, S3D_GROUP, LINE_MODEL), s3d(ribbons(16.0, LINE_WIDTH, "x"), b"NAM grading line")),
        ((S3D_TYPE, S3D_GROUP, DASH_MODEL), s3d(ribbons(DASH_LENGTH, DASH_WIDTH, "y"), b"NAM grading dash")),
        ((EFFDIR_TYPE, EFFDIR_GROUP, WIREFRAME_EFFECTS), wireframe_effects()),
    ]


def main():
    entries = wireframe_entries()
    for mask in sorted(set(masks())):
        alphas = [alpha(mask, row, column) for row in range(SIZE) for column in range(SIZE)]
        entries.append(((FSH_TYPE, GROUP, mask), fsh(dxt3(alphas))))
        if len(sys.argv) > 2:
            png(f"{sys.argv[2]}/outline_{mask:02x}.png", alphas)
    with open(sys.argv[1], "wb") as f:
        f.write(dbpf(entries))
    print(f"wrote {len(entries)} entries to {sys.argv[1]}")


if __name__ == "__main__":
    main()
