# Tongue v2: evidence

Model: `shadow-final-v2.pt` (MobileNetV3-small blocks 0..8, ImageNet init, light mask head), trained
on 168 labelled capture frames from 21 static pose bursts plus three live headset sessions (tongue
poses, closed mouth, smile with teeth). Masks are approximate (hand polygons; live poses use the
previous model's masks on plainly visible tongues). Old positive frames are sampled 3x.

## Whole-session cross-validation (each capture session held out entirely)

Presence counts with the 20-cell rule used during model selection; direction is the position in
[-1, 1] (negative = user's left).

| Held-out set | correct | position (min / median / max) |
|---|---|---|
| 9 old hidden-tongue sessions (closed, ajar, open, wide, smile, synced false-open...) | 72/72 | - |
| live closed mouth | 48/51 | |
| left extreme / half / side | 8/8, 8/8, 8/8 | -1.00..-0.69, -0.90..-0.80, -0.79..-0.70 |
| right extreme / half / fresh / 02 | 6/8, 8/8, 8/8, 8/8 | 0.94..1.00, 0.60..0.75, 0.98..1.00, 0.60..1.00 |
| center (two sessions) | 16/16 | -0.56..-0.16 before recalibration; centre angle then set to 0.66 |
| live pose session | 130/150 | full range |
| smile with teeth, when NOT in training | 8/135 | (only fixed by including it) |

The first capture session (saturated, different headset fit) is not detected; it is excluded from
claims.

## Release threshold (v2.1): 100 mask cells

On the author's current headset fit, false masks on lips (half-pucker, wide-open, lower lip pushed
out, closed) were 20-80 cells (90% under 80), real tongues 95+ (sessions: 99% / 100% of frames).
The threshold was raised from 20 to 100. Cost: small extreme tongues from the old, different
headset fit (32-53 cells) would no longer count, and the tongue appears about one frame later.

## Pucker

PICO's own LipPucker is used on the PC: while it is above 0.3 (full at 0.5, held 400 ms after it
drops) the image model cannot extend the tongue. Retraining with pucker frames was tried (three
variants) and rejected: each lost extreme left/right or brought back the open-mouth false tongue.

## Up/down: not implemented

Two head-mounts with up/down poses were recorded. Straight up and straight down separate on the
centre line, but down-left / down-right look like "up" on one mount, and ordinary left/right poses
from another mount look like "up" by every simple shape measure tried (normalised area, mask width).
A learned head with several more independent mounts would be needed.
