#!/usr/bin/env python3
"""Animate Scott's hand-drawn Boeing sketch as the gadget's idle state:
gentle breathing (slow vertical swell, bottom-anchored) plus periodic
eye blinks. Renders a faithful RLCD panel simulation for approval;
on approval the same frames become the firmware idle bank.
"""
import math
import pathlib

from PIL import Image, ImageDraw, ImageFilter

HERE = pathlib.Path(__file__).resolve().parent
SRC = pathlib.Path.home() / "workspace/user/media_library/image/image.png"
W, H = 56, 59
FRAMES = 75
BLINKS = [(18, 3), (54, 3)]  # (start frame, duration) — two blinks/loop


def clean(im):
    g = im.convert("L")
    # Flatten the paper's uneven lighting: divide by a heavily blurred
    # copy so the paper goes uniformly white and pencil stays dark.
    bg = g.filter(ImageFilter.GaussianBlur(60))
    gp, bp = g.load(), bg.load()
    w, h = g.size
    out = Image.new("L", (w, h))
    op = out.load()
    for y in range(h):
        for x in range(w):
            b = bp[x, y] or 1
            op[x, y] = max(0, min(255, int(gp[x, y] * 255 / b)))
    lut = []
    for v in range(256):
        t = (v - 60) * 255 / (250 - 60)
        lut.append(max(0, min(255, int(t))))
    return out.point(lut)


def bird_bbox(g):
    px = g.load()
    w, h = g.size
    minx, miny, maxx, maxy = w, h, 0, 0
    for y in range(h):
        for x in range(w):
            if px[x, y] < 200:
                minx = min(minx, x)
                maxx = max(maxx, x)
                miny = min(miny, y)
                maxy = max(maxy, y)
    return minx, miny, maxx, maxy


def main():
    g = clean(Image.open(SRC))
    # Crop and eye boxes read off the cleaned sketch by eye (grid pass):
    # bird spans ~(135,120)-(830,920) of the 1024px source.
    box = (75, 90, 75 + 816, 90 + 860)
    crop = g.crop(box)
    print("crop:", box, crop.size)
    cw_, ch_ = crop.size
    # Eyes in art coordinates (x, y, w, h): left eye is full and round;
    # the right eye is partly behind the beak in this 3/4 view.
    eye_art = [(23, 14, 4, 5), (31, 14, 4, 5)]

    def frame_art(i):
        phase = math.sin(2 * math.pi * i / FRAMES)
        sy = 1 + 0.02 * phase
        sx = 1 + 0.008 * phase
        w2, h2 = int(cw_ * sx), int(ch_ * sy)
        r = crop.resize((w2, h2), Image.LANCZOS)
        canvas = Image.new("L", (cw_, ch_), 255)
        canvas.paste(r, ((cw_ - w2) // 2, ch_ - h2))  # bottom-anchored
        art = canvas.resize((W, H), Image.LANCZOS)
        # Firm up the pencil: gamma so mid-grey strokes dither denser.
        return art.point(lambda v: int(255 * (v / 255) ** 1.3))

    blink_frames = set()
    for start, dur in BLINKS:
        blink_frames.update(range(start, start + dur))

    arts = []
    for i in range(FRAMES):
        art = frame_art(i)
        if i in blink_frames:
            d = ImageDraw.Draw(art)
            apx = art.load()
            for ex, ey, ew, eh in eye_art:
                # Lid: fill the eye with the tone just above it, then a
                # gentle downward arc line across the middle.
                # The face around the eyes is blank paper in the
                # sketch, so the lid is paper with a dark arc.
                fill = 255
                d.rectangle([ex, ey, ex + ew - 1, ey + eh - 1],
                            fill=int(fill))
                midy = ey + eh // 2
                for k in range(ew):
                    arc = int(0.6 * math.sin(math.pi * k / max(1, ew - 1)))
                    d.point((ex + k, midy + arc), fill=30)
        arts.append(art)

    # Panel simulation: the sketch is greyscale, so the data palette is
    # inverted luminance; ink where data lum beats the Bayer threshold.
    bayer = [[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
             [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
             [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
             [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]]
    SCALE = 5
    OX, OY = (400 - W * SCALE) // 2, (300 - H * SCALE) // 2

    def panel(art):
        img = Image.new("L", (400, 300), 255)
        pp = img.load()
        ap = art.load()
        for cy in range(H):
            for cx in range(W):
                lum = 255 - ap[cx, cy]  # inverted data luminance
                for dy in range(SCALE):
                    for dx in range(SCALE):
                        x, y = OX + cx * SCALE + dx, OY + cy * SCALE + dy
                        if lum > bayer[y & 7][x & 7] * 4 + 2:
                            pp[x, y] = 0
        return img

    sims = [panel(a) for a in arts]
    sims[0].save(HERE / "preview_sketch.png")
    sims[0].save(HERE / "preview_sketch.gif", save_all=True,
                 append_images=sims[1:], duration=80, loop=0)
    arts[0].resize((280, 295), Image.NEAREST).save("/tmp/sketch_art0.png")
    print("previews written; blink frames:", sorted(blink_frames))


if __name__ == "__main__":
    main()
