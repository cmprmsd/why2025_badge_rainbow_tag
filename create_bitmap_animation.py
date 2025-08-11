#!/usr/bin/env python3
# create_bitmap_animation.py — Sprite sheet generator with diagonal rainbow + 3D warp
# Guarantees: when --bpp 8, palette index 0 (dark gray) is used ONLY for background.
#
# Example:
#   python create_bitmap_animation.py --text "@cmprmsd" --font /path/DejaVuSans-Bold.ttf \
#       --out sheet.bmp --cols 8 --rows 4 --frames 32 --size 160x160 --bpp 8 --scroll-cycles 1

import argparse, math, colorsys
from PIL import Image, ImageDraw, ImageFont
import numpy as np

# Background / transparency key color (dark gray, avoids any overlap with magenta)
KEY_BG = (32, 32, 32)

def parse_size(s):
    w, h = s.lower().split("x")
    return int(w), int(h)

def best_fit_text_rgba(text, font_path, max_w, max_h, pad=2):
    lo, hi, best = 8, 512, None
    # Find largest size that fits
    while lo <= hi:
        mid = (lo + hi) // 2
        font = ImageFont.truetype(font_path, size=mid)
        # Get exact bbox of drawn text at (0,0)
        temp_img = Image.new("L", (max_w, max_h), 0)
        draw = ImageDraw.Draw(temp_img)
        bbox = draw.textbbox((0, 0), text, font=font)
        w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
        if w + 2*pad <= max_w and h + 2*pad <= max_h:
            best = (mid, bbox)
            lo = mid + 1
        else:
            hi = mid - 1

    if best is None:
        best = (12, (0, 0, 12, 12))

    size, bbox = best
    font = ImageFont.truetype(font_path, size=size)
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]

    # Render exactly to bbox size + pad
    img = Image.new("RGBA", (w + 2*pad, h + 2*pad), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    draw.text((pad - bbox[0], pad - bbox[1]), text, font=font, fill=(255, 255, 255, 255))

    # Trim any extra transparent space (safety)
    arr = np.array(img)
    alpha = arr[..., 3]
    non_empty = np.argwhere(alpha > 0)
    if non_empty.size > 0:
        y0, x0 = non_empty.min(axis=0)
        y1, x1 = non_empty.max(axis=0) + 1
        img = img.crop((x0, y0, x1, y1))

    return img


def find_perspective_coeffs(dst_quad, src_quad):
    A, B = [], []
    for (xd, yd), (xs, ys) in zip(dst_quad, src_quad):
        A.append([xd, yd, 1, 0, 0, 0, -xs*xd, -xs*yd])
        A.append([0, 0, 0, xd, yd, 1, -ys*xd, -ys*yd])
        B.append(xs); B.append(ys)
    A = np.array(A, dtype=float)
    B = np.array(B, dtype=float)
    return np.linalg.lstsq(A, B, rcond=None)[0].tolist()

def rotate_xy(points, ax, ay):
    cx, sx = math.cos(ax), math.sin(ax)
    cy, sy = math.cos(ay), math.sin(ay)
    out = []
    for x, y, z in points:
        y1 = y*cx - z*sx
        z1 = y*sx + z*cx
        x2 = x*cy + z1*sy
        z2 = -x*sy + z1*cy
        out.append((x2, y1, z2))
    return out

def project(points, cx, cy, f):
    out = []
    for x, y, z in points:
        d = f + z
        if d < 1e-3: d = 1e-3
        s = f / d
        out.append((cx + x*s, cy + y*s))
    return out

# === Diagonal rainbow source (never outputs exact magenta) ===
def diagonal_rainbow_rgba(size, phase=0.0, avoid_transparent=True):
    """
    45° HSV rainbow (S=V=1).
    If avoid_transparent, nudge exact magenta off 255,0,255.
    (Not strictly needed when the background key is dark gray, but harmless.)
    """
    w, h = size
    x = np.arange(w)[None, :]          # shape (1, w)
    y = np.arange(h)[:, None]          # shape (h, 1)
    u = (x + y) / max(1, (w - 1) + (h - 1))  # shape (h, w), 0..1 along x+y diagonal
    H = (u + phase) % 1.0

    H8 = np.uint8(H * 255)
    S8 = np.full_like(H8, 255, dtype=np.uint8)
    V8 = np.full_like(H8, 255, dtype=np.uint8)

    hsv = np.dstack((H8, S8, V8))      # shape (h, w, 3)
    rgb = Image.fromarray(hsv, mode="HSV").convert("RGBA")

    if avoid_transparent:
        arr = np.array(rgb, dtype=np.uint8)
        # (255,0,255,*) -> (255,0,254,*)
        m = (arr[...,0]==255) & (arr[...,1]==0) & (arr[...,2]==255)
        arr[m, 2] = 254
        rgb = Image.fromarray(arr, "RGBA")
    return rgb


def generate_frames(text_rgba, frame_size, n_frames, ax_deg, ay_deg, focal,
                    alpha_thresh, scroll_cycles, avoid_transparent):
    W, H = frame_size
    src_w, src_h = text_rgba.size
    src_rect = [(0,0),(src_w,0),(src_w,src_h),(0,src_h)]
    obj = [(-src_w/2, -src_h/2, 0), (src_w/2, -src_h/2, 0),
           (src_w/2,  src_h/2, 0), (-src_w/2,  src_h/2, 0)]
    cx, cy = W/2, H/2
    ax_amp, ay_amp = math.radians(ax_deg), math.radians(ay_deg)

    glyph_alpha = text_rgba.split()[3]

    frames = []
    for i in range(n_frames):
        t = i / n_frames
        ax = ax_amp * math.sin(2*math.pi*t)
        ay = ay_amp * math.cos(2*math.pi*t)
        quad = project(rotate_xy(obj, ax, ay), cx, cy, focal)
        coeffs = find_perspective_coeffs(quad, src_rect)

        # Rainbow in source space; optional scroll
        phase = (t * scroll_cycles) % 1.0 if scroll_cycles != 0 else 0.0
        rainbow = diagonal_rainbow_rgba(text_rgba.size, phase=phase, avoid_transparent=avoid_transparent)
        rainbow.putalpha(glyph_alpha)

        warped = rainbow.transform((W, H), Image.PERSPECTIVE, coeffs, resample=Image.BILINEAR)
        alpha = warped.split()[3]
        mask = alpha.point(lambda a: 255 if a >= alpha_thresh else 0, 'L')

        frame = Image.new("RGB", (W, H), KEY_BG)
        frame.paste(warped.convert("RGB"), (0,0), mask)
        frames.append(frame)
    return frames

def pack_sheet(frames, cols, rows):
    assert len(frames) <= cols*rows
    fw, fh = frames[0].size
    sheet = Image.new("RGB", (cols*fw, rows*fh), KEY_BG)
    for idx, im in enumerate(frames):
        r = idx // cols
        c = idx % cols
        sheet.paste(im, (c*fw, r*fh))
    return sheet

# === Palette helpers for compact 8-bit output (index 0 = KEY_BG only) ===
def build_rainbow_palette():
    """Return a 256*3 list for Image.putpalette().
    Index 0 = KEY_BG (transparent), 1..255 = rainbow hues.
    Avoid exact (255,0,255) inside the rainbow range."""
    pal = [KEY_BG[0], KEY_BG[1], KEY_BG[2]]  # index 0 reserved for background
    for i in range(1, 256):
        h = i / 255.0
        r, g, b = [int(255*v) for v in colorsys.hsv_to_rgb(h, 1.0, 1.0)]
        # Avoid exact magenta in non-zero indices (purely for safety with magenta-sensitive tools)
        if (r, g, b) == (255, 0, 255):
            b = 254
        pal.extend([r, g, b])
    return pal  # length 768

def quantize_to_fixed_palette(img_rgb, palette_rgb_list):
    """Quantize to a fixed palette (no dithering) and return P-mode image."""
    pal_img = Image.new("P", (1, 1))
    pal_img.putpalette(palette_rgb_list)
    return img_rgb.quantize(palette=pal_img, dither=Image.Dither.NONE)

def force_background_index0(p_img, orig_rgb, palette):
    """Ensure only true background is index 0."""
    # Identify background pixels in the original RGB (exact KEY_BG)
    arr_rgb = np.array(orig_rgb)
    bg = (arr_rgb[...,0]==KEY_BG[0]) & (arr_rgb[...,1]==KEY_BG[1]) & (arr_rgb[...,2]==KEY_BG[2])

    arr_p = np.array(p_img)
    # Set background to 0
    arr_p[bg] = 0

    # Safety: if any non-background got 0, push it to a nonzero palette index (use 1)
    mask_err = (~bg) & (arr_p == 0)
    if np.any(mask_err):
        arr_p[mask_err] = 1

    out = Image.fromarray(arr_p, "P")
    out.putpalette(palette)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--text", default="@cmprmsd")
    ap.add_argument("--font", required=True)
    ap.add_argument("--out", default="sheet.bmp")
    ap.add_argument("--cols", type=int, default=8)
    ap.add_argument("--rows", type=int, default=4)
    ap.add_argument("--frames", type=int, default=32)
    ap.add_argument("--size", default="160x160")
    ap.add_argument("--ax", type=float, default=15.0)
    ap.add_argument("--ay", type=float, default=40.0)
    ap.add_argument("--focal", type=float, default=320.0)
    ap.add_argument("--alpha-thresh", type=int, default=32)
    ap.add_argument("--bpp", type=int, choices=(24, 8), default=24,
                    help="BMP bit depth: 24 (default) or 8 (paletted).")
    ap.add_argument("--scroll-cycles", type=float, default=0.0,
                    help="Rainbow scroll cycles per full loop (0 = static).")
    ap.add_argument(
        "--avoid-transparent",
        dest="avoid_transparent",
        action="store_true",
        default=True,
        help="Avoid producing exact magenta inside glyphs (harmless with dark-gray key; on by default)."
    )
    ap.add_argument(
        "--no-avoid-transparent",
        dest="avoid_transparent",
        action="store_false",
        help="Allow exact magenta in glyphs."
    )
    args = ap.parse_args()

    W,H = parse_size(args.size)
    if args.frames > args.cols*args.rows:
        raise SystemExit("frames must be <= cols*rows")

    text_rgba = best_fit_text_rgba(args.text, args.font, int(W*0.9), int(H*0.55), pad=4)
    frames = generate_frames(
        text_rgba, (W, H), args.frames,
        args.ax, args.ay, args.focal,
        max(0, min(255, args.alpha_thresh)),
        args.scroll_cycles,
        args.avoid_transparent
    )

    sheet = pack_sheet(frames, args.cols, args.rows)

    if args.bpp == 8:
        palette = build_rainbow_palette()
        # Quantize using fixed palette
        sheet_p = quantize_to_fixed_palette(sheet, palette)
        # Force only true background to index 0; never glyphs
        sheet_p = force_background_index0(sheet_p, sheet, palette)
        sheet_p.save(args.out, format="BMP")  # 8-bit paletted BMP
    else:
        # Windows BMP with 24-bit pixels (BI_RGB, uncompressed).
        sheet.save(args.out, format="BMP", bits=24)

    print(f"Wrote {args.out}  size={sheet.size}  frames={args.frames} "
          f"({args.cols}x{args.rows})  bpp={args.bpp}  scroll_cycles={args.scroll_cycles}")

if __name__ == "__main__":
    main()
