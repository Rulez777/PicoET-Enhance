"""Labels for live headset sessions. Negatives: empty masks. Positives: masks from the
accepted model (shadow-final.pt, prob>.5) on frames where the tongue is plainly visible
(owner-performed poses, contact sheets reviewed). Transitions are skipped, not guessed."""
from pathlib import Path
import sys
import numpy as np
here = Path(__file__).resolve().parent
A = here / 'sessions/device-20261026-230954'   # 45s poses: 0-5.8s closed mouth, 6.6-45s tongue poses
B = here / 'sessions/device-20261026-231223'   # 15s smile with teeth, no tongue
C = here / 'sessions/device-20261026-233416'   # 60s re-mounted: pucker 7-8.5/10-11.5/36-51s, relaxed 3-6/52-60s, poses 13.5-34s
fa = np.fromfile(A / 'frames.u8', np.uint8).reshape(-1, 200, 200); pa = np.load(A / 'prob.npy')
fb = np.fromfile(B / 'frames.u8', np.uint8).reshape(-1, 200, 200)
fps = len(fa) / 45
F, M, L, G, P = [], [], [], [], []
for i in range(0, int(5.8 * fps), 2):
    F.append(fa[i]); M.append(np.zeros((56, 56), np.float32)); L.append('hidden_closed_live'); G.append('LiveA'); P.append(False)
for i in range(int(6.6 * fps), len(fa), 4):
    m = (pa[i] > .5).astype(np.float32)
    if m.sum() >= 100:
        F.append(fa[i]); M.append(m); L.append('pose_live'); G.append('LiveA'); P.append(True)
for i in range(0, len(fb), 2):
    F.append(fb[i]); M.append(np.zeros((56, 56), np.float32)); L.append('hidden_smile_teeth'); G.append('LiveB-smile'); P.append(False)
fc = np.fromfile(C / 'frames.u8', np.uint8).reshape(-1, 200, 200); pc = np.load(C / 'prob-v2.npy'); fpc = len(fc) / 60
for a, b, lab in ((7, 8.5, 'hidden_pucker_live'), (10, 11.5, 'hidden_pucker_live'), (36, 51, 'hidden_pucker_live'),
                  (0, 6, 'hidden_relaxed_live'), (52, 60, 'hidden_relaxed_live')):
    for i in range(int(a * fpc), min(int(b * fpc), len(fc)), 2):
        F.append(fc[i]); M.append(np.zeros((56, 56), np.float32)); L.append(lab); G.append('LiveC'); P.append(False)
for i in range(int(13.5 * fpc), int(34 * fpc), 3):
    m = (pc[i] > .5).astype(np.float32)
    if m.sum() >= 100:
        F.append(fc[i]); M.append(m); L.append('pose_live_c'); G.append('LiveC'); P.append(True)
np.savez_compressed(here / 'extra-data.npz', frames=np.stack(F), masks56=np.stack(M), labels=np.array(L),
                    groups=np.array(G), positive=np.array(P))
print({g: int((np.array(G) == g).sum()) for g in sorted(set(G))})
