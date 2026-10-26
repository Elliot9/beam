"""Regenerates res/beam.ico and the Linux app icons (needs Pillow): python tools/make_icon.py"""
from pathlib import Path

from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "res" / "beam.ico"
LINUX_SIZES = [48, 64, 128, 256]
LINUX_DIR = ROOT / "data" / "linux" / "icons"  # <size>/local.beam.Launcher.png (hicolor theme)


def render(size: int) -> Image.Image:
    s = size * 8  # supersample, then downscale for clean edges
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    # diagonal red -> orange gradient
    grad = Image.new("RGBA", (s, s))
    px = grad.load()
    for y in range(s):
        for x in range(s):
            t = (x + (s - y)) / (2 * s)
            r = int(255 * (1 - t) + 255 * t)
            g = int(78 * (1 - t) + 150 * t)
            b = int(86 * (1 - t) + 60 * t)
            px[x, y] = (r, g, b, 255)
    mask = Image.new("L", (s, s), 0)
    margin = s * 0.06
    ImageDraw.Draw(mask).rounded_rectangle([margin, margin, s - margin, s - margin], radius=s * 0.24, fill=255)
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)
    w = s * 0.13  # the "beam": a bold diagonal stroke with a light source dot
    d.line([(s * 0.30, s * 0.70), (s * 0.72, s * 0.28)], fill=(255, 255, 255, 255), width=int(w))
    for cx, cy in [(s * 0.30, s * 0.70), (s * 0.72, s * 0.28)]:
        d.ellipse([cx - w / 2, cy - w / 2, cx + w / 2, cy + w / 2], fill=(255, 255, 255, 255))
    d.ellipse([s * 0.22 - w * 0.15, s * 0.78 - w * 0.15, s * 0.22 + w * 0.15, s * 0.78 + w * 0.15],
              fill=(255, 255, 255, 230))
    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    images = [render(sz) for sz in SIZES]
    images[-1].save(OUT, format="ICO", sizes=[(sz, sz) for sz in SIZES], append_images=images[:-1])
    print(f"wrote {OUT}")
    by_size = dict(zip(SIZES, images))
    for size in LINUX_SIZES:
        path = LINUX_DIR / str(size) / "local.beam.Launcher.png"
        path.parent.mkdir(parents=True, exist_ok=True)
        (by_size.get(size) or render(size)).save(path, format="PNG")
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
