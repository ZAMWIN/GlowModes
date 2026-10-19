#!/usr/bin/env python3
"""Draw the mod's logo.png (the icon Geode shows in the mod list).

Written by hand with Pillow instead of a generative model on purpose: the icon is
flat geometry, it has to stay readable at 40 px in the mod list, and this way the
file is reproducible from the repository.

    python tools/make-logo.py            # writes logo.png next to mod.json
    python tools/make-logo.py --size 512 # any square size

The design is the mod in one picture: a plain cube icon (the player) sitting in a
rainbow aura -- per-gamemode glow, sweeping every hue.
"""

from __future__ import annotations

import argparse
import colorsys
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

SS = 4  # supersampling factor; everything is drawn big and scaled down

BG_TOP = (14, 14, 30)
BG_BOTTOM = (27, 27, 51)
BORDER = (51, 51, 92)
CUBE = (245, 247, 255)
CUBE_OUTLINE = (16, 16, 32)


def rounded_mask(size: int, box: tuple[float, float, float, float], radius: float) -> Image.Image:
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    return mask


def vertical_gradient(size: int, top: tuple[int, int, int], bottom: tuple[int, int, int]) -> Image.Image:
    grad = Image.new("RGB", (1, size))
    grad.putdata([
        tuple(round(top[c] + (bottom[c] - top[c]) * (y / max(size - 1, 1))) for c in range(3))
        for y in range(size)
    ])
    return grad.resize((size, size), Image.NEAREST)


def rainbow_ring(size: int, phase_deg: float = 0.0) -> Image.Image:
    """A full colour wheel: hue by the angle around the centre, 1.0 saturation."""
    centre = (size - 1) / 2.0
    px: list[tuple[int, int, int]] = []
    for y in range(size):
        dy = y - centre
        for x in range(size):
            dx = x - centre
            hue = ((math.degrees(math.atan2(dy, dx)) + 90.0 + phase_deg) % 360.0) / 360.0
            r, g, b = colorsys.hsv_to_rgb(hue, 1.0, 1.0)
            px.append((round(r * 255), round(g * 255), round(b * 255)))
    ring = Image.new("RGB", (size, size))
    ring.putdata(px)
    return ring


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=256, help="output size in pixels (square)")
    ap.add_argument("--out", type=Path, default=None, help="output file (default logo.png next to mod.json)")
    args = ap.parse_args()

    size = args.size
    w = size * SS
    out = args.out or (Path(__file__).resolve().parent.parent / "logo.png")

    # Background: dark rounded square with a slightly lighter frame.
    logo = vertical_gradient(w, BG_TOP, BG_BOTTOM).convert("RGBA")
    body = rounded_mask(w, (0, 0, w - 1, w - 1), radius=w * 0.18)
    logo.putalpha(body)
    ImageDraw.Draw(logo).rounded_rectangle(
        (w * 0.012, w * 0.012, w * 0.988, w * 0.988),
        radius=w * 0.17, outline=BORDER, width=max(SS, round(w * 0.012)),
    )

    # The aura: the cube's silhouette, blurred twice (a tight core and a wide
    # halo), coloured with the colour wheel and screened on top of the gradient.
    cube_size = w * 0.34
    cube_box = ((w - cube_size) / 2, (w - cube_size) / 2, (w + cube_size) / 2, (w + cube_size) / 2)
    cage = rounded_mask(w, cube_box, radius=cube_size * 0.16)

    ring = rainbow_ring(size, phase_deg=20.0).resize((w, w), Image.BICUBIC)
    for blur, strength in ((w * 0.035, 1.0), (w * 0.090, 0.55)):
        glow_mask = cage.filter(ImageFilter.GaussianBlur(blur)).point(lambda v: round(v * strength))
        glow = ring.copy()
        glow.putalpha(glow_mask)
        logo.alpha_composite(glow)

    # The cube itself: GD-icon-like, solid with a thick dark outline and a small
    # inner square so it reads as an icon and not as a blank tile.
    draw = ImageDraw.Draw(logo)
    outline = max(SS, round(w * 0.030))
    draw.rounded_rectangle(cube_box, radius=cube_size * 0.16, fill=CUBE, outline=CUBE_OUTLINE, width=outline)
    inner = cube_size * 0.42
    inner_box = ((w - inner) / 2, (w - inner) / 2, (w + inner) / 2, (w + inner) / 2)
    draw.rounded_rectangle(
        inner_box, radius=inner * 0.22, fill=(60, 62, 96), outline=CUBE_OUTLINE, width=max(SS, round(w * 0.012)),
    )

    final = logo.resize((size, size), Image.LANCZOS)
    final.save(out)
    print(f"wrote {out} ({size}x{size})")


if __name__ == "__main__":
    main()
