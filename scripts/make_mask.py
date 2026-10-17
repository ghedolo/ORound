"""Converts an image into the pool floor file for drop_16M_pool_image.

The image is scaled to the given width and centered on a square of MASK_SIZE x MASK_SIZE
pixels (the display plus a margin for the refraction shift).

Mask mode (default): one byte per pixel, figure coverage 0..16, taken from the alpha
channel or from dark pixels when the image has no transparency; drawn with the palette.
Color mode (--rgb): three bytes per pixel (RGB888), drawn with the image colors.
The firmware tells these two modes apart by the file size.
Animation mode (--anim, animated GIF): frames composited on a white background, scaled
so the GIF canvas is <width> px wide and centered on the display, cropped to the box where
any frame differs from the background. --step=N keeps one frame every N (times summed),
--bg=RRGGBB sets the background color (default white).
File: header "FLA2", then box width, height, frame count, box x0, y0 in display+margin
coordinates (uint16 little endian each), background index (uint8), color count (uint8,
0 = 256), palette RGB888, frame times in ms (uint16 each), then one byte per pixel
(palette index) for each frame of the box.

Option --crop=x0,y0,x1,y1 crops the source image first (pixel box).

Usage: python scripts/make_mask.py <image> <output.bin> [width in px] [--rgb | --anim [--step=N]] [--crop=x0,y0,x1,y1]
"""
import struct
import sys

from PIL import Image

LCD = 466
MARGIN = 24          # >= max refraction shift in px, so shifted reads stay inside
MASK_SIZE = LCD + 2 * MARGIN
LEVELS = 16


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rgb = "--rgb" in sys.argv
    if "--anim" in sys.argv:
        step = 1
        bg = (255, 255, 255)
        for a in sys.argv:
            if a.startswith("--step="):
                step = int(a[7:])
            if a.startswith("--bg="):
                v = a[5:].lstrip("#")
                bg = (int(v[0:2], 16), int(v[2:4], 16), int(v[4:6], 16))
        make_anim(args[0], args[1], int(args[2]) if len(args) > 2 else 440, step, bg=bg)
        return
    src, dst = args[0], args[1]
    width = int(args[2]) if len(args) > 2 else 400
    img = Image.open(src)
    for a in sys.argv:
        if a.startswith("--crop="):
            img = img.crop(tuple(int(v) for v in a[7:].split(",")))
    if rgb:
        img = img.convert("RGB")
        height = round(img.height * width / img.width)
        img = img.resize((width, height), Image.LANCZOS)
        canvas = Image.new("RGB", (MASK_SIZE, MASK_SIZE), (0, 0, 0))
        canvas.paste(img, ((MASK_SIZE - width) // 2, (MASK_SIZE - height) // 2))
        with open(dst, "wb") as f:
            f.write(canvas.tobytes())
        print(f"{dst}: {MASK_SIZE}x{MASK_SIZE} RGB888, image {width}x{height}")
        return
    if img.mode in ("RGBA", "LA") or "transparency" in img.info:
        cover = img.convert("RGBA").getchannel("A")
    else:
        cover = Image.eval(img.convert("L"), lambda v: 255 - v)
    height = round(cover.height * width / cover.width)
    cover = cover.resize((width, height), Image.LANCZOS)
    mask = Image.new("L", (MASK_SIZE, MASK_SIZE), 0)
    mask.paste(cover, ((MASK_SIZE - width) // 2, (MASK_SIZE - height) // 2))
    data = bytes(round(v * LEVELS / 255) for v in mask.tobytes())
    with open(dst, "wb") as f:
        f.write(data)
    print(f"{dst}: {MASK_SIZE}x{MASK_SIZE}, figure {width}x{height}")


def make_anim(src, dst, width, step=1, colors=64, bg=(255, 255, 255)):
    from PIL import ImageChops

    img = Image.open(src)
    n = getattr(img, "n_frames", 1)
    frames = []
    durations = []
    for i in range(0, n, step):
        img.seek(i)
        comp = Image.new("RGBA", img.size, bg + (255,))
        comp.alpha_composite(img.convert("RGBA"))
        f = comp.convert("RGB")
        height = round(f.height * width / f.width)
        frames.append(f.resize((width, height), Image.LANCZOS))
        t = 0
        for k in range(i, min(i + step, n)):
            img.seek(k)
            t += img.info.get("duration", 100)
        durations.append(t)
    # Box where any frame differs from the background, plus 2 px
    plain = Image.new("RGB", frames[0].size, bg)
    box = None
    for f in frames:
        b = ImageChops.difference(f, plain).convert("L").point(lambda v: 255 if v > 8 else 0).getbbox()
        if b:
            box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]), max(box[2], b[2]), max(box[3], b[3]))
    box = (max(box[0] - 2, 0), max(box[1] - 2, 0), min(box[2] + 2, width), min(box[3] + 2, frames[0].height))
    frames = [f.crop(box) for f in frames]
    w, h = frames[0].size
    x0 = (MASK_SIZE - width) // 2 + box[0]
    y0 = (MASK_SIZE - plain.height) // 2 + box[1]
    assert 0 <= x0 and x0 + w <= MASK_SIZE and 0 <= y0 and y0 + h <= MASK_SIZE, "box outside the display area"
    # Shared palette from all frames stacked vertically, background color included
    strip = Image.new("RGB", (w, h * len(frames) + 1), bg)
    for i, f in enumerate(frames):
        strip.paste(f, (0, i * h))
    pal_img = strip.quantize(colors=colors, method=Image.MEDIANCUT)
    palette = pal_img.getpalette()[: colors * 3]
    data = [f.quantize(palette=pal_img, dither=Image.Dither.NONE).tobytes() for f in frames]
    bg_index = Image.new("RGB", (1, 1), bg).quantize(palette=pal_img, dither=Image.Dither.NONE).tobytes()[0]
    with open(dst, "wb") as out:
        out.write(b"FLA2")
        out.write(struct.pack("<HHHHHBB", w, h, len(frames), x0, y0, bg_index, colors % 256))
        out.write(bytes(palette))
        out.write(struct.pack(f"<{len(durations)}H", *durations))
        for d in data:
            out.write(d)
    total = sum(len(d) for d in data)
    print(f"{dst}: {len(frames)} frames, box {w}x{h} at ({x0},{y0}), {colors} colors, {total} bytes, times {sorted(set(durations))} ms")


if __name__ == "__main__":
    main()
