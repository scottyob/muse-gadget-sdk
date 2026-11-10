#!/usr/bin/env python3
"""Boeing animation v10: Scott's own GIFs as the firmware banks.

idle-bird.gif  -> idle bank (normal state)
bird-yapping.gif -> talk bank (Speaking state)

Each GIF is normalized by its own bird bounding box so both banks land
on exactly the same spot in the 56x59 art (no jump when the state
changes), resampled onto the firmware's fixed 40 ms/frame cadence,
quantized to one shared palette, and tone-inverted for the RLCD panel.
Writes avatar/happy_anim.c/.h and panel previews.
"""
import pathlib

from PIL import Image, ImageFilter

HERE = pathlib.Path(__file__).resolve().parent
MEDIA = pathlib.Path.home() / "workspace/user/media_library/image"
SDK_AVATAR = HERE.parent / "muse-gadget-sdk-rlcd42-minimal" / "esp32" / "avatar"
W, H = 56, 59
TICK_MS = 40
# Where the bird's bbox lands in art space: rows 4..57 (head clear of
# the title band), cols 3..52.
ART_BOX = (3, 4, 53, 58)  # x0, y0, x1, y1 inclusive


def clean(g):
    bg = g.filter(ImageFilter.GaussianBlur(40))
    gp, bp = g.load(), bg.load()
    w, h = g.size
    out = Image.new("L", (w, h))
    op = out.load()
    for y in range(h):
        for x in range(w):
            op[x, y] = max(0, min(255, int(gp[x, y] * 255 / (bp[x, y] or 1))))
    lut = [max(0, min(255, int((v - 60) * 255 / 190))) for v in range(256)]
    return out.point(lut)


def load_gif(path):
    im = Image.open(path)
    frames, durs = [], []
    for i in range(im.n_frames):
        im.seek(i)
        frames.append(clean(im.convert("L")))
        durs.append(im.info.get("duration", 100) or 100)
    return frames, durs


def bird_bbox(g):
    px = g.load()
    w, h = g.size
    m = int(w * 0.03)
    minx, miny, maxx, maxy = w, h, 0, 0
    for y in range(m, h - m):
        for x in range(m, w - m):
            if px[x, y] < 170:
                minx = min(minx, x)
                maxx = max(maxx, x)
                miny = min(miny, y)
                maxy = max(maxy, y)
    return minx, miny, maxx, maxy


def normalize(frames):
    """Crop so the bird bbox lands in ART_BOX; resize to W x H."""
    bx0, by0, bx1, by1 = bird_bbox(frames[len(frames) // 2])
    bw, bh = bx1 - bx0 + 1, by1 - by0 + 1
    ax0, ay0, ax1, ay1 = ART_BOX
    # Source pixels per art pixel, driven by the tighter fit.
    spu = max(bw / (ax1 - ax0 + 1), bh / (ay1 - ay0 + 1))
    cw, ch = int(W * spu), int(H * spu)
    cx0 = int(bx0 - ax0 * spu)
    cy0 = int(by0 - ay0 * spu)
    out = []
    for f in frames:
        canvas = Image.new("L", (f.size[0] + cw, f.size[1] + ch), 255)
        canvas.paste(f, (cw // 2, ch // 2))
        c = canvas.crop((cw // 2 + cx0, ch // 2 + cy0,
                         cw // 2 + cx0 + cw, ch // 2 + cy0 + ch))
        art = c.resize((W, H), Image.LANCZOS)
        out.append(art.point(lambda v: int(255 * (v / 255) ** 1.3)))
    return out


def to_ticks(frames, durs):
    """Resample a GIF timeline onto 40 ms ticks (nearest active frame)."""
    total = sum(durs)
    ticks = max(1, round(total / TICK_MS))
    starts = []
    t = 0
    for d in durs:
        starts.append(t)
        t += d
    out = []
    for k in range(ticks):
        tmid = (k + 0.5) * TICK_MS
        idx = 0
        for i, s in enumerate(starts):
            if s <= tmid:
                idx = i
        out.append(frames[idx])
    return out


def stretch_invert_lut():
    lut = []
    for v in range(256):
        t = (v - 128) * 1.25 + 128
        t = max(0, min(255, t))
        lut.append(255 - int(t))
    return lut


def rgb565_be(rgb):
    r, g, b = rgb
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((v & 0xFF) << 8) | (v >> 8)


def main():
    idle_frames, idle_durs = load_gif(MEDIA / "idle-bird.gif")
    talk_frames, talk_durs = load_gif(MEDIA / "bird-yapping.gif")
    print("idle:", len(idle_frames), "frames", idle_durs,
          "bbox", bird_bbox(idle_frames[0]))
    print("talk:", len(talk_frames), "frames", talk_durs,
          "bbox", bird_bbox(talk_frames[0]))

    idle_art = to_ticks(normalize(idle_frames), idle_durs)
    talk_art = to_ticks(normalize(talk_frames), talk_durs)
    print("ticks: idle", len(idle_art), "talk", len(talk_art))

    # Shared palette from both banks (greyscale art -> grey palette).
    strip = Image.new("RGB", (W * 8, H * 4))
    samples = idle_art[::7] + talk_art[::3]
    for i, im in enumerate(samples[:32]):
        strip.paste(im.convert("RGB"), ((i % 8) * W, (i // 8) * H))
    pal_img = strip.quantize(colors=24, method=Image.MEDIANCUT)
    pal = pal_img.getpalette()[:24 * 3]
    pal = [tuple(pal[i:i + 3]) for i in range(0, 24 * 3, 3)]

    def to_indices(im):
        q = im.convert("RGB").quantize(palette=pal_img, dither=Image.NONE)
        return list(q.getdata())

    idle_idx = [to_indices(a) for a in idle_art]
    talk_idx = [to_indices(a) for a in talk_art]
    used = sorted({v for bank in (idle_idx, talk_idx) for f in bank
                   for v in f})
    remap = {old: new for new, old in enumerate(used)}
    idle_idx = [[remap[v] for v in f] for f in idle_idx]
    talk_idx = [[remap[v] for v in f] for f in talk_idx]
    lut = stretch_invert_lut()
    palette_data = [tuple(lut[c] for c in pal[old]) for old in used]
    print("palette colours:", len(palette_data))

    def fmt_frames(name, bank):
        lines = [f"const uint8_t {name}"
                 f"[{len(bank)}][HAPPY_ANIM_HEIGHT * HAPPY_ANIM_WIDTH] = {{"]
        for f in bank:
            lines.append("  {")
            for y in range(H):
                lines.append("    " + ", ".join(
                    str(f[y * W + x]) for x in range(W)) + ",")
            lines.append("  },")
        lines.append("};")
        return lines

    c = ["// Copyright (c) Meta Platforms, Inc. and affiliates.",
         "",
         "// Generated for the RLCD-4.2 Boeing port from Scott's own",
         "// idle-bird.gif and bird-yapping.gif (tone-inverted for the",
         "// panel; see workspace/boeing-anim/make_anim3.py).",
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
    print("happy_anim written")

    # Panel previews.
    bayer = [[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
             [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
             [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
             [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]]
    lums = [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in palette_data]
    SCALE = 5
    OX, OY = (400 - W * SCALE) // 2, (300 - H * SCALE) // 2

    def panel(idx_frame):
        img = Image.new("L", (400, 300), 255)
        pp = img.load()
        for cy in range(H):
            for cx in range(W):
                lum = lums[idx_frame[cy * W + cx]]
                for dy in range(SCALE):
                    for dx in range(SCALE):
                        x, y = OX + cx * SCALE + dx, OY + cy * SCALE + dy
                        if lum > bayer[y & 7][x & 7] * 4 + 2:
                            pp[x, y] = 0
        return img

    idle_sim = [panel(f) for f in idle_idx]
    idle_sim[0].save(HERE / "preview_idle_v10.png")
    idle_sim[0].save(HERE / "preview_idle_v10.gif", save_all=True,
                     append_images=idle_sim[1:], duration=80, loop=0)
    talk_sim = [panel(f) for f in talk_idx]
    talk_sim[0].save(HERE / "preview_talk_v10.gif", save_all=True,
                     append_images=talk_sim[1:], duration=80, loop=0)
    print("previews written")


if __name__ == "__main__":
    main()
