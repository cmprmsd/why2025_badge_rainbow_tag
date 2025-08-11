#!/usr/bin/env python3
# preview_sheet.py — loop a BMP sprite sheet as an animation
# Usage:
#   python preview_sheet.py --sheet sheet.bmp --cols 8 --rows 4 --fps 24 --scale 2
#
# Notes:
# - Treats MAGENTA (255,0,255) as transparent over a checkerboard (default),
#   or pick a solid bg: --bg "#202020" or --bg magenta or --bg checker
# - Keys: space = pause/resume, left/right = step, q/esc = quit

import argparse, time
from PIL import Image, ImageTk
import tkinter as tk

MAGENTA = (255, 0, 255)

def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sheet", required=True, help="path to BMP/PNG sprite sheet")
    ap.add_argument("--cols", type=int, required=True)
    ap.add_argument("--rows", type=int, required=True)
    ap.add_argument("--frames", type=int, default=None, help="defaults to cols*rows")
    ap.add_argument("--fps", type=float, default=24.0)
    ap.add_argument("--scale", type=int, default=2, help="integer upscaling factor")
    ap.add_argument("--bg", default="checker", help='checker | magenta | hex (e.g. "#222222")')
    return ap.parse_args()

def make_checker(w, h, cell=10):
    from PIL import ImageDraw
    img = Image.new("RGB", (w, h), (200, 200, 200))
    drw = ImageDraw.Draw(img)
    for y in range(0, h, cell):
        for x in range(0, w, cell):
            if ((x // cell) + (y // cell)) % 2 == 0:
                drw.rectangle([x, y, x + cell - 1, y + cell - 1], fill=(240, 240, 240))
    return img

def solid_bg(w, h, spec):
    if spec == "magenta":
        color = MAGENTA
    elif spec.startswith("#") and (len(spec) in (4, 7)):
        # #rgb or #rrggbb
        if len(spec) == 4:
            color = tuple(int(spec[i]*2, 16) for i in (1,2,3))
        else:
            color = tuple(int(spec[i:i+2], 16) for i in (1,3,5))
    else:
        # try named color via a tiny Tk parser
        root = tk.Tk(); root.withdraw()
        color = tuple(int(v) for v in root.winfo_rgb(spec))
        color = (color[0]//256, color[1]//256, color[2]//256)
        root.destroy()
    return Image.new("RGB", (w, h), color)

def extract_frames(sheet, cols, rows, n_frames=None):
    fw, fh = sheet.width // cols, sheet.height // rows
    frames = []
    total = cols * rows if n_frames is None else min(n_frames, cols*rows)
    for i in range(total):
        r, c = divmod(i, cols)
        crop = sheet.crop((c*fw, r*fh, (c+1)*fw, (r+1)*fh))
        frames.append(crop)
    return frames

def chroma_over_bg(frame_rgb, bg_rgb):
    """Treat MAGENTA as transparent and composite over background."""
    fr = frame_rgb.convert("RGBA")
    r, g, b, a = fr.split()
    # Create mask where magenta becomes transparent
    import numpy as np
    arr = np.array(fr)
    mask_opaque = ~((arr[...,0]==255) & (arr[...,1]==0) & (arr[...,2]==255))
    arr[...,3] = (mask_opaque * 255).astype("uint8")
    fr = Image.fromarray(arr, "RGBA")
    return Image.alpha_composite(bg_rgb.convert("RGBA"), fr).convert("RGB")

def main():
    args = parse_args()
    sheet = Image.open(args.sheet).convert("RGB")
    frames = extract_frames(sheet, args.cols, args.rows, args.frames)

    fw, fh = frames[0].size
    scale = max(1, int(args.scale))
    vw, vh = fw*scale, fh*scale

    # Build background
    if args.bg == "checker":
        bg = make_checker(fw, fh, cell=max(6, fw//16))
    else:
        bg = solid_bg(fw, fh, args.bg)

    # Pre-composite chroma and scale upfront for smooth playback
    prepared = []
    for fr in frames:
        comp = chroma_over_bg(fr, bg)
        if scale != 1:
            comp = comp.resize((vw, vh), Image.NEAREST)
        prepared.append(comp)

    # Tk setup
    root = tk.Tk()
    root.title(f"Preview — {args.sheet}")
    lbl = tk.Label(root)
    lbl.pack()
    tk_img = None

    state = {"i": 0, "paused": False, "last": time.time(), "spf": 1.0/max(1e-6, args.fps)}

    def render():
        nonlocal tk_img
        img = prepared[state["i"]]
        tk_img = ImageTk.PhotoImage(img)
        lbl.configure(image=tk_img)

    def tick():
        if not state["paused"]:
            now = time.time()
            if now - state["last"] >= state["spf"]:
                state["i"] = (state["i"] + 1) % len(prepared)
                state["last"] = now
                render()
        root.after(5, tick)

    def on_key(ev):
        k = ev.keysym.lower()
        if k in ("space",):
            state["paused"] = not state["paused"]
        elif k in ("right", "d"):
            state["i"] = (state["i"] + 1) % len(prepared); render()
        elif k in ("left", "a"):
            state["i"] = (state["i"] - 1) % len(prepared); render()
        elif k in ("q", "escape"):
            root.destroy()

    root.bind("<Key>", on_key)
    render()
    root.after(5, tick)
    root.mainloop()

if __name__ == "__main__":
    main()

