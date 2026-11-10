#!/usr/bin/env python3
"""Boeing hawk gadget animation from Scott's own idle-loop video.

Samples 75 frames across the full 7.12 s loop, crops tight on the head,
quantizes to a small palette, and emits:
  boeing_grid.gif  - the 300x295 / 5px-grid GIF gen_happy_anim.py expects
  preview.gif      - the 56x59 art at 3x, in colour
  preview_panel.gif- simulated RLCD render: greyscale + Bayer dither at 5x
"""
import subprocess, math, pathlib
from PIL import Image

SRC = pathlib.Path.home() / "workspace/avatars/avatar-1794016911769134567-1-call-light-idle-afd76594-5030-4656-abbd-0a43919d08bd-7x12-v62553407fe.mp4"
OUT = pathlib.Path.home() / "workspace/boeing-anim"
NFRAMES, W, H = 75, 56, 59

# --- pull all frames as PNGs via ffmpeg ---
tmp = OUT / "frames"; tmp.mkdir(exist_ok=True)
subprocess.run(["ffmpeg", "-v", "error", "-i", str(SRC), str(tmp / "f%03d.png")], check=True)
frames = sorted(tmp.glob("f*.png"))
print("video frames:", len(frames))

# --- crop: measured hawk bbox x27-420, y150-759; frame the head ---
mid = Image.open(frames[len(frames)//2]).convert("RGB")
x0, y0, side = 14, 140, 420
print("crop:", x0, y0, side)

# --- shared palette from the middle frame's art ---
def art_of(im):
    return im.crop((x0, y0, x0+side, y0+side)).resize((W, H), Image.LANCZOS)
base = art_of(mid)
pal_img = base.quantize(colors=24, method=Image.MEDIANCUT)

idx = [round(i*(len(frames)-1)/(NFRAMES-1)) for i in range(NFRAMES)]
grid_frames, prev_frames, panel_frames = [], [], []
bayer = [[0,8,2,10],[12,4,14,6],[3,11,1,9],[15,7,13,5]]
for fi in idx:
    art = art_of(Image.open(frames[fi]).convert("RGB")).quantize(palette=pal_img, dither=Image.NONE)
    rgb = art.convert("RGB")
    # The RLCD backend renders bright data as ink (the status UI is drawn
    # light-on-dark and flipped for paper), so a positive image comes out
    # as a negative. Pre-invert the art, with a contrast stretch so the
    # face separates on the 1-bit panel: data_lum = 255 - stretched(art_lum)
    # per channel (luminance is linear, so RGB inversion inverts lum).
    lut = []
    for v in range(256):
        t = (v - 128) * 1.25 + 128
        t = max(0, min(255, t))
        lut.append(255 - int(t))
    rgb = rgb.point(lut * 3)
    # generator input: 5px blocks, art at grid offset (4,0)
    grid = Image.new("RGB", (300, 295), (255, 255, 255))
    grid.paste(rgb.resize((W*5, H*5), Image.NEAREST), (4*5, 0))
    grid_frames.append(grid)
    prev_frames.append(rgb.resize((W*3, H*3), Image.NEAREST))
    # panel simulation of the (inverted) data: ink where data luminance
    # exceeds the 8x8 Bayer threshold, at 5x scale
    lum = rgb.convert("L")
    big = Image.new("L", (W*5, H*5), 255)
    bp = big.load(); lp = lum.load()
    b8 = [[0,32,8,40,2,34,10,42],[48,16,56,24,50,18,58,26],[12,44,4,36,14,46,6,38],
          [60,28,52,20,62,30,54,22],[3,35,11,43,1,33,9,41],[51,19,59,27,49,17,57,25],
          [15,47,7,39,13,45,5,37],[63,31,55,23,61,29,53,21]]
    for yy in range(H):
        for xx in range(W):
            on = 0 if lp[xx, yy] > b8[yy % 8][xx % 8]*4 + 2 else 255
            for dy in range(5):
                for dx in range(5):
                    bp[xx*5+dx, yy*5+dy] = on
    panel_frames.append(big)

grid_frames[0].save(OUT/"boeing_grid.gif", save_all=True, append_images=grid_frames[1:], duration=40, loop=0)
prev_frames[0].save(OUT/"preview.gif", save_all=True, append_images=prev_frames[1:], duration=40, loop=0)
panel_frames[0].save(OUT/"preview_panel.gif", save_all=True, append_images=panel_frames[1:], duration=40, loop=0)
g = Image.open(OUT/"boeing_grid.gif")
print("grid gif frames:", g.n_frames)
