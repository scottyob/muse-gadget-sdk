#!/usr/bin/env python3
"""Boeing hawk animation v9: idle bank + talking-beak bank for the RLCD-4.2.

Both banks share one palette. Art is tone-inverted (with a contrast
stretch) because the RLCD backend renders bright data as ink. The talk
bank is synthesized from idle frames: the lower beak drops as one piece
with a dark mouth wedge, following a chatty open/close envelope.

Writes the SDK's avatar/happy_anim.c/.h directly (same format as
tools/gen_happy_anim.py, plus the talk bank) and panel previews.
"""
import glob
import pathlib

from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
SDK_AVATAR = HERE.parent / "muse-gadget-sdk-rlcd42-minimal" / "esp32" / "avatar"
W, H = 56, 59
CROP = (14, 140, 420)  # x0, y0, side — same framing as v7/v8
IDLE_FRAMES = 75
TALK_FRAMES = 24
# Beak openness per talk frame (0 closed, 1 half, 2 full) — irregular,
# speech-like bursts rather than a metronome.
ENVELOPE = [0, 1, 2, 1, 0, 1, 2, 2, 1, 0, 1, 2,
            1, 2, 0, 0, 1, 2, 1, 0, 2, 1, 1, 0]
MOUTH = (70, 42, 30)


def stretch_invert_lut():
    lut = []
    for v in range(256):
        t = (v - 128) * 1.25 + 128
        t = max(0, min(255, t))
        lut.append(255 - int(t))
    return lut


def art_of(frame_img):
    x0, y0, side = CROP
    return frame_img.crop((x0, y0, x0 + side, y0 + side)).resize(
        (W, H), Image.LANCZOS)


def beak_open(art, shift):
    """Gape the beak itself: the lower beak drops as one rigid piece and
    the gap stays strictly inside the beak's own silhouette (each gap
    row only spans the columns the beak occupies in that row). No mouth
    is drawn beneath or outside the beak.
    """
    if shift == 0:
        return art
    px = art.load()
    pts = []
    for y in range(10, 40):
        for x in range(18, 40):
            r, g, b = px[x, y]
            if r > 170 and r - b > 120 and g - b > 80:
                pts.append((x, y))
    if len(pts) < 8:
        return art
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    x0, x1 = min(xs) - 2, max(xs) + 2
    split = max(ys) + 1            # first row below the cere
    bottom = min(H - 1, split + 3)  # dark tip + mouth line
    # Per-row extent of the beak body (cere, tip, outline: anything in
    # the beak box that is not plain feather/background tone).
    def beakish(x, y):
        r, g, b = px[x, y]
        lum = 0.299 * r + 0.587 * g + 0.114 * b
        return lum < 95 or (r - b > 60 and g - b > 20)
    extent = {}
    for y in range(split, bottom + 1):
        cols = [x for x in range(x0, x1 + 1) if beakish(x, y)]
        if cols:
            extent[y] = (min(cols), max(cols))
    out = art.copy()
    opx = out.load()
    lower = {}
    for y in range(split, bottom + 1):
        for x in range(x0, x1 + 1):
            lower[(x, y)] = px[x, y]
    # The gap: only the vacated rows, only within the beak's own
    # per-row extent (interpolated between the split rows' extents).
    gap_cols = extent.get(split, (x0 + 2, x1 - 2))
    for y in range(split, split + shift):
        for x in range(gap_cols[0], gap_cols[1] + 1):
            opx[x, y] = MOUTH
    for (x, y), c in lower.items():
        if y + shift < H:
            opx[x, y + shift] = c
    return out


def rgb565_be(rgb):
    r, g, b = rgb
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((v & 0xFF) << 8) | (v >> 8)


def main():
    frames = sorted(glob.glob(str(HERE / "frames" / "*.png")))
    print("video frames:", len(frames))
    src = [Image.open(f).convert("RGB") for f in frames]

    idle_src = [src[round(i * (len(src) - 1) / (IDLE_FRAMES - 1))]
                for i in range(IDLE_FRAMES)]
    talk_src = [src[round(i * (len(src) - 1) / (TALK_FRAMES - 1))]
                for i in range(TALK_FRAMES)]

    idle_pos = [art_of(im) for im in idle_src]
    shift_of = [0, 1, 2]  # envelope level -> beak drop in art pixels
    talk_pos = [beak_open(art_of(im), shift_of[ENVELOPE[i]])
                for i, im in enumerate(talk_src)]

    # One shared palette from both banks (positive art).
    strip = Image.new("RGB", (W * 8, H * 4))
    samples = idle_pos[::10] + talk_pos[::3]
    for i, im in enumerate(samples[:32]):
        strip.paste(im, ((i % 8) * W, (i // 8) * H))
    pal_img = strip.quantize(colors=24, method=Image.MEDIANCUT)
    palette_pos = pal_img.getpalette()[:24 * 3]
    palette_pos = [tuple(palette_pos[i:i + 3]) for i in range(0, 24 * 3, 3)]

    def to_indices(im):
        q = im.quantize(palette=pal_img, dither=Image.NONE)
        return list(q.getdata())

    idle_idx = [to_indices(im) for im in idle_pos]
    talk_idx = [to_indices(im) for im in talk_pos]
    used = sorted({v for bank in (idle_idx, talk_idx) for f in bank for v in f})
    remap = {old: new for new, old in enumerate(used)}
    idle_idx = [[remap[v] for v in f] for f in idle_idx]
    talk_idx = [[remap[v] for v in f] for f in talk_idx]

    lut = stretch_invert_lut()
    palette_data = [
        tuple(lut[c] for c in palette_pos[old]) for old in used]
    print("palette colours:", len(palette_data))

    def fmt_frames(name, bank):
        lines = [f"const uint8_t {name}"
                 f"[{len(bank)}][HAPPY_ANIM_HEIGHT * HAPPY_ANIM_WIDTH] = {{"]
        for f in bank:
            rows = []
            for y in range(H):
                rows.append("    " + ", ".join(
                    str(f[y * W + x]) for x in range(W)) + ",")
            lines.append("  {")
            lines.extend(rows)
            lines.append("  },")
        lines.append("};")
        return lines

    c = ["// Copyright (c) Meta Platforms, Inc. and affiliates.",
         "",
         "// Generated for the RLCD-4.2 Boeing port: idle + talking-beak",
         "// banks from Scott's Muse avatar idle loop (tone-inverted for",
         "// the panel; see workspace/boeing-anim/make_anim2.py).",
         '#include "happy_anim.h"',
         "",
         "#if CONFIG_HOMEHUB_DISPLAY",
         f"const uint16_t happy_anim_palette[{len(palette_data)}] = {{"]
    for rgb in palette_data:
        c.append(f"    0x{rgb565_be(rgb):04x},  // "
                 f"#{rgb[0]:02x}{rgb[1]:02x}{rgb[2]:02x}")
    c.append("};")
    c.append("")
    c.extend(fmt_frames("happy_anim_frames", idle_idx))
    c.append("")
    c.extend(fmt_frames("happy_anim_talk_frames", talk_idx))
    c.append("#endif")
    (SDK_AVATAR / "happy_anim.c").write_text("\n".join(c) + "\n")

    h = ["// Copyright (c) Meta Platforms, Inc. and affiliates.",
         "",
         "// Generated for the RLCD-4.2 Boeing port. Do not edit.",
         "#pragma once",
         "",
         "#include <stdint.h>",
         '#include "sdkconfig.h"',
         "",
         "#define HAPPY_ANIM_WIDTH     56",
         "#define HAPPY_ANIM_HEIGHT    59",
         f"#define HAPPY_ANIM_FRAMES    {len(idle_idx)}",
         f"#define HAPPY_ANIM_TALK_FRAMES {len(talk_idx)}",
         "#define HAPPY_ANIM_FRAME_MS  40",
         "",
         "#if CONFIG_HOMEHUB_DISPLAY",
         f"extern const uint16_t happy_anim_palette[{len(palette_data)}];",
         "extern const uint8_t happy_anim_frames"
         "[HAPPY_ANIM_FRAMES][HAPPY_ANIM_HEIGHT * HAPPY_ANIM_WIDTH];",
         "extern const uint8_t happy_anim_talk_frames"
         "[HAPPY_ANIM_TALK_FRAMES][HAPPY_ANIM_HEIGHT * HAPPY_ANIM_WIDTH];",
         "#endif"]
    (SDK_AVATAR / "happy_anim.h").write_text("\n".join(h) + "\n")
    print("happy_anim.c/.h written:", len(idle_idx), "idle +",
          len(talk_idx), "talk frames")

    # Panel previews: exact backend mapping at scale 5 on a 400x300
    # canvas — ink where data luminance beats the 8x8 Bayer threshold
    # at absolute panel coordinates.
    bayer = [[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
             [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
             [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
             [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]]
    lums = [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in palette_data]
    SCALE = 5
    OX, OY = (400 - W * SCALE) // 2, (300 - H * SCALE) // 2

    def panel_frame(idx_frame):
        img = Image.new("L", (400, 300), 255)
        px = img.load()
        for cy in range(H):
            for cx in range(W):
                lum = lums[idx_frame[cy * W + cx]]
                for dy in range(SCALE):
                    for dx in range(SCALE):
                        x, y = OX + cx * SCALE + dx, OY + cy * SCALE + dy
                        if lum > bayer[y & 7][x & 7] * 4 + 2:
                            px[x, y] = 0
        return img

    sim_idle = [panel_frame(idle_idx[i]) for i in (0, 30)]
    sim_idle[0].save(HERE / "preview_panel_v9.png")
    talk_sim = [panel_frame(f) for f in talk_idx]
    talk_sim[0].save(HERE / "preview_talk_v9.gif", save_all=True,
                     append_images=talk_sim[1:], duration=80, loop=0)
    print("previews written")


if __name__ == "__main__":
    main()
