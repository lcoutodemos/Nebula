"""Render the Nebula icon set (a dot nebula on a
dark plate), redrawn per size rather than downscaled so 16-48 px stay legible.

Proportions: a rounded-square plate with a small inset, a top-lit hairline rim matching the
app's glass panels, a soft core glow, and a Gaussian field of star dots whose count and size scale
with the target so small icons read as a few crisp points instead of grey noise.

    python tools\\make_icons.py            # writes src\\Nebula.Windows\\Assets
Deterministic: same seed, same pixels. Requires Pillow.
"""
from __future__ import annotations

import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ASSETS = Path(__file__).resolve().parents[1] / "src" / "Nebula.Windows" / "Assets"
SS = 4  # supersampling


def _lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def mark(size: int) -> Image.Image:
    """The square icon at `size` px (plate + nebula), transparent outside the plate."""
    S = size * SS
    inset = (0.035 if size <= 24 else 0.055) * S
    plate = S - 2 * inset
    radius = plate * 0.235
    box = (inset, inset, S - inset, S - inset)

    # Plate: vertical graphite gradient.
    grad = Image.new("RGBA", (S, S))
    top, bottom = (44, 44, 49), (17, 17, 19)
    gd = ImageDraw.Draw(grad)
    for y in range(S):
        gd.line([(0, y), (S, y)], fill=_lerp(top, bottom, y / max(1, S - 1)) + (255,))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(grad, (0, 0), mask)

    # Core glow: an elliptical soft light behind the stars.
    cx, cy = S / 2, inset + plate * 0.52
    glow = Image.new("L", (S, S), 0)
    gr = ImageDraw.Draw(glow)
    rx, ry = plate * 0.30, plate * 0.17
    gr.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=150 if size >= 32 else 190)
    glow = glow.filter(ImageFilter.GaussianBlur(plate * 0.11))
    out = Image.alpha_composite(out, Image.merge("RGBA", (Image.new("L", (S, S), 255),) * 3 + (Image.composite(glow, Image.new("L", (S, S), 0), mask),)))

    # Stars: Gaussian ellipse, brighter towards the centre; fewer and larger when small.
    rng = random.Random(1729 + size)
    if size >= 128:
        count, dot = 1300, plate * 0.0085
    elif size >= 64:
        count, dot = 300, plate * 0.012
    elif size >= 44:
        count, dot = 120, plate * 0.017
    elif size >= 32:
        count, dot = 46, plate * 0.024
    elif size >= 24:
        count, dot = 20, plate * 0.032
    else:
        count, dot = 9, plate * 0.045
    dot = max(dot, 0.55 * SS)
    stars = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(stars)
    sx, sy = plate * 0.19, plate * 0.11
    placed = 0
    while placed < count:
        u, v = rng.gauss(0, 1), rng.gauss(0, 1)
        if u * u + v * v > 6.5:
            continue
        x, y = cx + u * sx, cy + v * sy
        d = math.sqrt(u * u + v * v)
        a = int(255 * max(0.32, min(1.0, 1.05 - d * 0.28)))
        r = dot * (0.75 + 0.5 * rng.random())
        shade = 200 + int(55 * max(0.0, 1 - d / 2.5))
        sd.ellipse((x - r, y - r, x + r, y + r), fill=(shade, shade, shade, a))
        placed += 1
    out = Image.alpha_composite(out, Image.composite(stars, Image.new("RGBA", (S, S), (0, 0, 0, 0)), mask))

    # Rim: 1 px (at target size) hairline, bright at the top, almost gone at the bottom.
    rim_w = max(1, round(SS * (1 if size < 128 else size / 128)))
    rim_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(rim_mask).rounded_rectangle(box, radius=radius, outline=255, width=rim_w)
    fade = Image.new("L", (S, S))
    fd = ImageDraw.Draw(fade)
    for y in range(S):
        t = (y - inset) / max(1, plate)
        fd.line([(0, y), (S, y)], fill=int(255 * max(0.05, 0.30 - 0.25 * t)))
    rim_alpha = Image.composite(fade, Image.new("L", (S, S), 0), rim_mask)
    out = Image.alpha_composite(out, Image.merge("RGBA", (Image.new("L", (S, S), 255),) * 3 + (rim_alpha,)))

    return out.resize((size, size), Image.LANCZOS)


def centred(canvas: tuple[int, int], icon: int) -> Image.Image:
    img = Image.new("RGBA", canvas, (0, 0, 0, 0))
    m = mark(icon)
    img.paste(m, ((canvas[0] - icon) // 2, (canvas[1] - icon) // 2), m)
    return img


def main() -> None:
    ASSETS.mkdir(parents=True, exist_ok=True)
    for old in ASSETS.glob("*.png"):
        old.unlink()
    written = []

    def save(img: Image.Image, name: str):
        img.save(ASSETS / name)
        written.append(name)

    for scale, px in {100: 44, 125: 55, 150: 66, 200: 88, 400: 176}.items():
        save(mark(px), f"Square44x44Logo.scale-{scale}.png")
    for px in (16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 256):
        m = mark(px)
        save(m, f"Square44x44Logo.targetsize-{px}.png")
        save(m, f"Square44x44Logo.targetsize-{px}_altform-unplated.png")
        save(m, f"Square44x44Logo.targetsize-{px}_altform-lightunplated.png")
    for scale, px in {100: 150, 200: 300, 400: 600}.items():
        save(centred((px, px), round(px * 0.62)), f"Square150x150Logo.scale-{scale}.png")
    for scale, (w, h) in {100: (310, 150), 200: (620, 300), 400: (1240, 600)}.items():
        save(centred((w, h), round(h * 0.62)), f"Wide310x150Logo.scale-{scale}.png")
    for scale, (w, h) in {100: (620, 300), 200: (1240, 600), 400: (2480, 1200)}.items():
        save(centred((w, h), round(h * 0.42)), f"SplashScreen.scale-{scale}.png")
    for scale, px in {100: 50, 200: 100, 400: 200}.items():
        save(mark(px), f"StoreLogo.scale-{scale}.png")
    for scale, px in {100: 24, 200: 48, 400: 96}.items():
        save(mark(px), f"LockScreenLogo.scale-{scale}.png")

    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    frames = [mark(s) for s in sizes]
    frames[-1].save(ASSETS / "AppIcon.ico", sizes=[(s, s) for s in sizes], append_images=frames[:-1])
    written.append("AppIcon.ico")
    print(f"{len(written)} assets written to {ASSETS}")


if __name__ == "__main__":
    main()
