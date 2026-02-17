#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Iterable, Sequence

from PIL import Image, ImageDraw, ImageFont


WIDTH = 320
HEIGHT = 480
ROW_Y = (95, 113, 131, 149, 167, 185, 203, 221)
SELECTED_BLOCK_OFFSET_Y = 166
ROW_H = 14
TITLE_BAR_H = 14
TRANSPARENT_INDEX = 31


def load_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = (
        "C:/Windows/Fonts/eurostib.ttf",
        "C:/Windows/Fonts/micross.ttf",
        "C:/Windows/Fonts/arialbd.ttf",
        "C:/Windows/Fonts/arial.ttf",
    )
    for font_path in candidates:
        path = Path(font_path)
        if path.exists():
            try:
                return ImageFont.truetype(str(path), size=size)
            except OSError:
                continue
    return ImageFont.load_default()


def text_x_center(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.ImageFont) -> int:
    left, _top, right, _bottom = draw.textbbox((0, 0), text, font=font)
    width = right - left
    return (WIDTH - width) // 2


def draw_centered_text(
    draw: ImageDraw.ImageDraw,
    text: str,
    font: ImageFont.ImageFont,
    fill: int,
    x0: int,
    y0: int,
    x1: int,
    y1: int,
) -> None:
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    text_w = right - left
    text_h = bottom - top
    box_w = (x1 - x0) + 1
    box_h = (y1 - y0) + 1
    x = x0 + ((box_w - text_w) // 2) - left
    y = y0 + ((box_h - text_h) // 2) - top
    draw.text((x, y), text, font=font, fill=fill)


def create_palette() -> list[int]:
    palette = [0] * (256 * 3)

    def set_color(index: int, r: int, g: int, b: int) -> None:
        base = index * 3
        palette[base] = r
        palette[base + 1] = g
        palette[base + 2] = b

    # Common colors for a config-style menu sheet.
    set_color(0, 0, 0, 0)            # black bars
    set_color(1, 255, 255, 255)      # selected text
    set_color(2, 154, 154, 154)      # unselected text
    set_color(TRANSPARENT_INDEX, 255, 0, 255)  # magenta transparent key

    # Fill everything else with transparent key color to keep palette stable.
    for i in range(256):
        if i in (0, 1, 2, TRANSPARENT_INDEX):
            continue
        set_color(i, 255, 0, 255)

    return palette


def draw_rows(
    image: Image.Image,
    labels: Sequence[str],
    title: str,
) -> None:
    draw = ImageDraw.Draw(image)
    title_font = load_font(12)
    row_font = load_font(12)

    for base_y in (0, 240):
        title_y1 = base_y + TITLE_BAR_H - 1
        draw.rectangle((0, base_y, WIDTH - 1, title_y1), fill=0)
        draw_centered_text(draw, title, title_font, 1, 0, base_y, WIDTH - 1, title_y1)

    for row_index, row_y in enumerate(ROW_Y):
        label = labels[row_index]

        draw.rectangle((0, row_y, WIDTH - 1, row_y + ROW_H - 1), fill=0)
        draw_centered_text(draw, label, row_font, 2, 0, row_y, WIDTH - 1, row_y + ROW_H - 1)

        selected_y = row_y + SELECTED_BLOCK_OFFSET_Y
        draw.rectangle((0, selected_y, WIDTH - 1, selected_y + ROW_H - 1), fill=0)
        draw_centered_text(draw, label, row_font, 1, 0, selected_y, WIDTH - 1, selected_y + ROW_H - 1)


def encode_dat(image: Image.Image) -> bytes:
    if image.mode != "P":
        raise ValueError("Image must be paletted (mode 'P').")

    width, height = image.size
    if width != WIDTH or height != HEIGHT:
        raise ValueError(f"Expected {WIDTH}x{HEIGHT}, got {width}x{height}.")

    palette = image.getpalette()
    if palette is None:
        raise ValueError("Paletted image has no palette.")
    if len(palette) < 256 * 3:
        palette = palette + [0] * (256 * 3 - len(palette))

    payload = bytearray()
    payload.append(255)  # 256-color palette header (payload starts at 3*255 + 1 = 766)

    # File stores colors as B, G, R triplets.
    for i in range(256):
        r = palette[i * 3 + 0]
        g = palette[i * 3 + 1]
        b = palette[i * 3 + 2]
        payload.extend((b, g, r))

    payload.extend((width & 0xFF, (width >> 8) & 0xFF, height & 0xFF, (height >> 8) & 0xFF))

    pixels = image.tobytes()
    for y in range(height - 1, -1, -1):
        start = y * width
        payload.extend(pixels[start : start + width])

    return bytes(payload)


def parse_labels(raw: str | None) -> list[str]:
    default_labels = [
        "HOST",
        "JOIN",
        "CHANGE NICKNAME",
        "ADDRESS",
        "PORT",
        "",
        "",
        "RETURN TO TITLE",
    ]
    if not raw:
        return default_labels

    labels = [entry.strip().upper() for entry in raw.split(",") if entry.strip()]
    if len(labels) != 8:
        raise ValueError("Expected exactly 8 comma-separated labels.")
    return labels


def save_outputs(out_dir: Path, basename: str, image: Image.Image) -> tuple[Path, Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    png_path = out_dir / f"{basename}.png"
    bmp_path = out_dir / f"{basename}.bmp"
    dat_path = out_dir / f"{basename}.dat"

    image.save(png_path, format="PNG")
    image.save(bmp_path, format="BMP")
    dat_path.write_bytes(encode_dat(image))
    return png_path, bmp_path, dat_path


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate a config-style EFZ netplay object sheet and .dat file.")
    parser.add_argument("--out-dir", default="assets", help="Output directory.")
    parser.add_argument("--basename", default="netplay_ob", help="Output filename base.")
    parser.add_argument("--title", default="NETPLAY SETTINGS", help="Menu title text.")
    parser.add_argument(
        "--labels",
        default=None,
        help="Comma-separated list of 8 menu labels. Default: HOST,JOIN,...",
    )
    args = parser.parse_args(list(argv) if argv is not None else None)

    labels = parse_labels(args.labels)
    image = Image.new("P", (WIDTH, HEIGHT), TRANSPARENT_INDEX)
    image.putpalette(create_palette())
    draw_rows(image, labels, args.title.upper())

    png_path, bmp_path, dat_path = save_outputs(Path(args.out_dir), args.basename, image)
    print(f"Generated: {png_path}")
    print(f"Generated: {bmp_path}")
    print(f"Generated: {dat_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
