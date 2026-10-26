"""Shared shadow-pipeline model and geometry. Training, offline replay and the live PC
runner all import this file so the preprocessing cannot drift between them.

Input contract: one uint8 grayscale frame, 200x200, produced on the headset by 2x2
averaging of the 400x400 mouth image (rounded half up). Offline frames go through
`downsample400` to get the identical bytes."""
import numpy as np
import torch
from torch import nn
from torch.nn import functional as F
from torchvision.models import mobilenet_v3_small

SIDE = 200          # transmitted frame side
RES = 224           # model input side (same as the validated spatial224 experiment)
OUT = RES // 4      # mask grid side, 56
MIN_AREA = 100     # headset presence rule (v2.1): lips give 20-80 cells, real tongue 95+
NOSE = .94          # saturated nose threshold used by geometry_probe.py

# Angle (radians, image coords, tongue centroid relative to nose centroid) calibration taken
# from whole-capture-excluded predictions of finetuned-spatial-v2-session, not training fits:
# left .21-.46, center .68-.89, right .98-1.23.
ANGLE_LEFT, ANGLE_CENTER, ANGLE_RIGHT = .33, .80, 1.11          # shadow-final.pt (accepted 20Hz build)
# shadow-final-v2.pt (live smile/closed negatives), from its session-held-out CV:
# left .19-.47, center .53-.70, right .99-1.29.
CALIBRATION = {'shadow-final.pt': (.33, .80, 1.11), 'shadow-final-v2.pt': (.33, .66, 1.11)}
DEADZONE = .06
# Physically possible tongue directions (all real poses seen: .13-1.48 rad).
ANGLE_MIN, ANGLE_MAX = 0.0, 1.7


def downsample400(frame400: np.ndarray) -> np.ndarray:
    """uint8 400x400 -> uint8 200x200, bit-identical to the headset's (a+b+c+d+2)>>2."""
    f = frame400.astype(np.uint16)
    s = f[0::2, 0::2] + f[1::2, 0::2] + f[0::2, 1::2] + f[1::2, 1::2]
    return ((s + 2) >> 2).astype(np.uint8)


class Segment(nn.Module):
    """MobileNetV3-small blocks 0..8 + small mask head (finetune_spatial_cv.py architecture)."""

    def __init__(self, pretrained=True):
        super().__init__()
        from torchvision.models import MobileNet_V3_Small_Weights
        weights = MobileNet_V3_Small_Weights.DEFAULT if pretrained else None
        self.encoder = mobilenet_v3_small(weights=weights).features[:9]
        # Light head for the headset CPU budget: 1x1 mix then 3x3 context (~12M MAC vs 95M).
        self.head = nn.Sequential(nn.Conv2d(89, 16, 1), nn.ReLU(),
                                  nn.Conv2d(16, 16, 3, padding=1), nn.ReLU(), nn.Conv2d(16, 1, 1))

    def forward(self, x):
        gray = x.sqrt()
        x = gray.repeat(1, 3, 1, 1)
        x = (x - x.new_tensor([.485, .456, .406])[None, :, None, None]) / x.new_tensor([.229, .224, .225])[None, :, None, None]
        parts = [F.interpolate(gray, (OUT, OUT), mode='bilinear', align_corners=False)]
        for i, b in enumerate(self.encoder):
            x = b(x)
            if i in (1, 3, 8):
                parts.append(F.interpolate(x, (OUT, OUT), mode='bilinear', align_corners=False))
        return self.head(torch.cat(parts, 1))


def to_input(frames200: torch.Tensor) -> torch.Tensor:
    """uint8 (N,200,200) tensor -> float (N,1,224,224) in [0,1]."""
    x = frames200.float()[:, None] / 255
    return F.interpolate(x, (RES, RES), mode='bilinear', align_corners=False)


def geometry(frame200: np.ndarray, prob56: np.ndarray):
    """Returns dict(present, area, angle, position). position in [-1,1], negative = user's left."""
    mask = prob56 > .5
    area = int(mask.sum())
    res = dict(present=area >= MIN_AREA, area=area, angle=None, position=0.0)
    nose = frame200.astype(np.float32) / 255 >= NOSE
    if not res['present'] or nose.sum() < 50:
        res['present'] = res['present'] and nose.sum() >= 50
        return res
    yy, xx = np.indices(nose.shape)
    nx, ny = xx[nose].mean(), yy[nose].mean()
    # mask grid -> frame coordinates (cell centres)
    my, mx = np.nonzero(mask)
    w = prob56[my, mx]
    scale = frame200.shape[0] / prob56.shape[0]
    tx = ((mx + .5) * scale * w).sum() / w.sum()
    ty = ((my + .5) * scale * w).sum() / w.sum()
    angle = float(np.arctan2(ty - ny, tx - nx))
    if not (ANGLE_MIN < angle < ANGLE_MAX):     # mask level with/above the nose: not a tongue
        res['present'] = False
        return res
    res['angle'] = angle
    res['position'] = angle_to_position(angle)
    return res


def angle_to_position(angle: float) -> float:
    if angle < ANGLE_CENTER:
        p = (angle - ANGLE_CENTER) / (ANGLE_CENTER - ANGLE_LEFT)
    else:
        p = (angle - ANGLE_CENTER) / (ANGLE_RIGHT - ANGLE_CENTER)
    p = max(-1.0, min(1.0, p))
    if abs(p) < DEADZONE:
        return 0.0
    return (abs(p) - DEADZONE) / (1 - DEADZONE) * (1 if p > 0 else -1)
