# Tongue left/right for PICO 4 Enterprise (PicoET-Enhance + UnifiedPicoModule)

PICO's own face tracking reports only `TongueOut`. This adds **TongueLeft / TongueRight** (and a
model-based TongueOut fallback) from the mouth camera, computed **on the headset**, delivered to
VRCFaceTracking over Wi-Fi. No ADB, no PC-side image processing, no firewall rules.

> **Status: works for its author, personal model.** The network was trained on frames from one
> person's face. It may work for others, but expect misses or false tongues on a different face or
> headset fit. The training tools are included so you can retrain on your own frames (see below).

## How it works

```
mouth camera (400x400, inside pxreyetrackingservice)
  -> payload copies a 2x2-averaged 200x200 frame at <=20 Hz (tracking thread does nothing else)
  -> worker thread on the big cores 4-7 (off the tracking service's little cores 0-3)
       MobileNetV3-small blocks 0..8 + small mask head, float32 C++ port, ~30 ms/frame
       presence: mask >= 100 cells of 56x56, tongue not level with / above the nose glare
       direction: angle of the tongue mask centroid around the nose glare centroid
       20 Hz output stage: on after 2 frames, off after 2, ~40 ms smoothing
  -> UDP 19031: the PC module sends "TSUB" once a second, the headset answers with 24-byte
     TNG2 packets (left, right, out, confidence) to that PC for 3 s
UnifiedPicoModule (VRCFaceTracking)
  -> TongueOut = max(PICO native TongueOut, model fallback)
  -> model fallback is blocked while PICO reports lip pucker (>0.3, full at 0.5, held 400 ms)
  -> TongueLeft / TongueRight scaled by TongueOut
```

Face and eye tracking keep their 90/90 Hz targets: the tracking thread only does a frame copy at
20 Hz, and inference runs on cores the tracking service is not allowed to use.

## Install

1. Root (Magisk) and PicoET-Enhance's firmware requirements apply unchanged (see README).
2. Install `magisk-picoet-enhance-v2.4.2-tongue.1.zip` in Magisk, reboot. Tongue is on by default
   (`/data/local/tmp/tongue-live.ip`; delete it to switch tongue off).
3. On the PC, replace `UnifiedPicoModule.dll` in your VRCFaceTracking `CustomLibs` module folder
   with the one from the release (built from UnifiedPicoModule v0.4.0 + `Sdk/TongueReceiver.cs`).
   It uses the PicoFacialBridge headset IP you already configured.
4. Start PicoFacialBridge as usual. `tongue-status.json` next to the DLL shows `"transport":"UDP"`
   and `"fresh":true` once packets arrive.

Avatars need TongueLeft/TongueRight (Unified Expressions) to show the direction.

## Verification done

- C++ port vs PyTorch on all 168 reference frames, on the headset: max logit difference 1e-3,
  0 differing mask cells, 0 differing presence/direction decisions.
- Model checked with whole recording sessions held out (never frame splits): extreme/half/side
  left and right poses detected, 0 false tongues on the hidden-tongue sessions it was checked on;
  per-session numbers are in `research/tongue-v2/README.md`.
- Owner tests in VRChat: left/right/center, smile with teeth, pucker and its release, wide-open
  mouth, open mouth with the lower lip pushed out, closed mouth, reboot without ADB.

## Retraining on your face

`research/tongue-v2/training/` holds the exact pipeline: `shadow_model.py` (shared preprocessing,
model, geometry), `train_shadow.py` / `train_shadow_v2.py` (whole-session cross-validation and
final training), `build_extra.py` (labels for recorded sessions), `export_headset_model.py`
(generates `hook/src/tongue_seg_weights.h` with the angle calibration). Record frames with the
payload's shadow mode (`/data/local/tmp/tongue-shadow.arm` saves 200x200 frames to
`/data/local/tmp/tongue-shadow.frames`), label sessions, retrain, export, rebuild the payload, and
check `tongue-seg-check` on the headset against PyTorch before installing.

The author's training frames are attached to the release as `tongue-training-data.zip`.
