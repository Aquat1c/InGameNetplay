from pathlib import Path


def parse_dat(path: Path):
    data = path.read_bytes()
    first = data[0]
    offsets = []
    for off in [1 + first * 3, 769, 766, *range(760, 773)]:
        if off not in offsets:
            offsets.append(off)

    best = None
    for off in offsets:
        if off + 4 > len(data):
            continue
        width = data[off] | (data[off + 1] << 8)
        height = data[off + 2] | (data[off + 3] << 8)
        pixel_offset = off + 4
        pixel_count = width * height
        if pixel_count == 0 or pixel_offset + pixel_count > len(data):
            continue
        score = 0
        if pixel_offset + pixel_count == len(data):
            score += 10000
        if width == 320:
            score += 100
        if height in (240, 480):
            score += 100
        if off in (766, 769):
            score += 50
        if best is None or score > best[0]:
            best = (score, off, width, height, pixel_offset)

    if best is None:
        raise RuntimeError(f"Could not parse {path}")

    _, header_offset, width, height, pixel_offset = best
    palette_bytes = data[1:header_offset]
    pixels_bottom_up = data[pixel_offset:pixel_offset + width * height]
    return {
        "width": width,
        "height": height,
        "header_offset": header_offset,
        "palette_bytes": palette_bytes,
        "pixels_bottom_up": pixels_bottom_up,
    }


def normalize_to_engine_255_palette_layout(path: Path):
    parsed = parse_dat(path)
    palette = bytearray(parsed["palette_bytes"])

    # EFZ's loadCompressedImageFile() skips exactly (3 * first_byte + 1)
    # bytes before reading width/height. For first_byte == 255 that means:
    #   1 byte header + 255 * 3 palette bytes = width/height at offset 766.
    expected_palette_bytes = 255 * 3
    if len(palette) > expected_palette_bytes:
        extra = len(palette) - expected_palette_bytes
        if extra == 3:
            # Some malformed variants carry one extra RGB triplet before the
            # width/height header. Drop that tail triplet so the engine sees
            # the header at offset 766.
            palette = palette[:expected_palette_bytes]
        else:
            raise RuntimeError(f"{path} has too many palette bytes: {len(palette)}")
    while len(palette) < expected_palette_bytes:
        # Fill missing tail entries with magenta so unused colors stay transparent-friendly.
        palette.extend((255, 0, 255))
    palette = palette[:expected_palette_bytes]

    out = bytearray()
    out.append(255)
    out.extend(palette)
    out.append(parsed["width"] & 0xFF)
    out.append((parsed["width"] >> 8) & 0xFF)
    out.append(parsed["height"] & 0xFF)
    out.append((parsed["height"] >> 8) & 0xFF)
    out.extend(parsed["pixels_bottom_up"])
    path.write_bytes(out)
    print(
        f"normalized {path} to size={len(out)} width={parsed['width']} height={parsed['height']} header={1 + 255 * 3}"
    )


if __name__ == "__main__":
    normalize_to_engine_255_palette_layout(Path("assets/battle_log_icons.dat"))
