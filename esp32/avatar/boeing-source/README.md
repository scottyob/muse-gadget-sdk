# Boeing animation — source art

`../happy_anim.c` / `../happy_anim.h` (the animation compiled into the
RLCD-4.2 firmware) are generated from the art in this folder. It is
tracked here so the firmware tree is self-contained: the C arrays are
not the only copy of Boeing.

- `boeing-sketch.png` — Scott's hand-drawn pencil-sketch Boeing.
- `idle-bird.gif` — Scott's idle animation of the sketch (9 frames,
  breathing/blinking): the firmware's idle bank.
- `bird-yapping.gif` — Scott's talking animation (6 frames, the whole
  beak gaping inside its silhouette): the firmware's talk bank, shown
  while Muse is speaking.
- `make_anim4.py` — the current generator. It normalizes both GIFs by
  the bird's bounding box to identical placement, resamples them onto
  the firmware's 40 ms cadence as tick->frame maps over the 15 distinct
  poses, quantizes to one shared palette, tone-inverts for the RLCD
  panel (bright data renders as ink), and writes `../happy_anim.c/.h`
  at 140x147, drawn at scale 2. Its `MEDIA` and `SDK_AVATAR` paths are
  written for the workspace layout it was developed in; adjust them to
  wherever the GIFs and this tree live.
- `make_anim.py`, `make_anim2.py`, `make_anim3.py`,
  `make_sketch_anim.py` — earlier generators (the pixel-hawk bank, the
  rejected synthesized talk bank, the first sketch banks), kept for
  history.
