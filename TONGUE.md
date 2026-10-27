# Tongue left/right for PICO 4 Enterprise (PicoET-Enhance + UnifiedPicoModule)

PICO's own face tracking reports only `TongueOut`. This adds **TongueLeft / TongueRight** (and a
model-based TongueOut fallback) from the mouth camera, computed **on the headset**, delivered to
VRCFaceTracking over Wi-Fi. No ADB, no PC-side image processing, no firewall rules.

> The shipped network was trained on one person's face. A different face or headset fit can read
> the direction wrongly: run the **Tongue Calibration** app (20 seconds). If the tongue is still not
> seen, its **Retrain** option teaches the model on your face, on the headset itself.

## Install (two files)

1. **Headset:** install `magisk-picoet-enhance-v2.4.2-tongue.2.zip` in Magisk and reboot. This
   installs everything on the headset, including the **Tongue Calibration** app (in the apps list).
   PicoET-Enhance's root and firmware requirements apply unchanged (see README).
2. **PC:** replace `UnifiedPicoModule.dll` in your VRCFaceTracking `CustomLibs` module folder with
   the one from the release (UnifiedPicoModule 0.4.0 + tongue receiver). It uses the
   PicoFacialBridge headset IP you already configured.

Start PicoFacialBridge as usual. Avatars need TongueLeft/TongueRight (Unified Expressions).
Tongue is on by default; to switch it off delete `/data/local/tmp/tongue-live.ip` (root).

## Calibration app

Open **Tongue Calibration** with the headset on:

- **Calibrate** (recommended, about 20 seconds, steps advance by themselves): headset fit check,
  closed mouth, open mouth without tongue, tongue straight out, to your left and slightly down, to
  your right and slightly down. It saves your tongue angles and a size threshold between your
  no-tongue mouth and your tongue, then shows a live left/right test.
- **Retrain (advanced)**, only if calibration did not help (tongue not seen on one side, or false
  tongues): records the same poses for about 15 seconds, then trains the model's mask head on the
  headset for 1-2 minutes. The new model is kept only if it tests better than the current one on
  held-out frames; otherwise nothing changes. A poor recording can make tracking worse, so the
  app warns first.
- **Restore standard** brings back the shipped model and calibration at any time.

After re-seating the headset differently, a quick re-calibration may help: the tongue's angle in
the camera depends on how the headset sits.

## How it works

```
mouth camera (400x400, inside pxreyetrackingservice)
  -> payload copies a 2x2-averaged 200x200 frame at <=20 Hz (tracking thread does nothing else)
  -> worker thread on the big cores 4-7 (off the tracking service's little cores 0-3)
       MobileNetV3-small blocks 0..8 + small mask head, float32 C++ port, ~30 ms/frame
       presence: mask >= threshold cells of 56x56 (default 100; stays on down to 60% once shown),
                 tongue not level with / above the nose glare
       direction: angle of the tongue mask centroid around the nose glare centroid, mapped with
                  the per-user calibration (/data/local/tmp/tongue-calibration)
       20 Hz output stage: on after 2 frames, off after 2, ~40 ms smoothing
  -> UDP 19031: the PC module sends "TSUB" once a second, the headset answers with 24-byte
     TNG2 packets (left, right, out, confidence) to that PC for 3 s
  -> 127.0.0.1 only: camera + mask stream and calibration/retraining commands for the app
UnifiedPicoModule (VRCFaceTracking)
  -> TongueOut = max(PICO native TongueOut, model fallback)
  -> model fallback is blocked while PICO reports lip pucker (>0.3, full at 0.5, held 400 ms)
  -> TongueLeft / TongueRight scaled by TongueOut
```

Face and eye tracking keep their 90/90 Hz targets: the tracking thread only does a frame copy at
20 Hz, and inference and retraining run on cores the tracking service is not allowed to use.

On-device retraining (`hook/src/tongue_train.h`): the encoder stays frozen; the 3-layer mask head
(3,777 weights) is trained from the shipped weights with Adam, BCE (positive weight 3) + Dice.
Tongue masks are built automatically (frame change against the open-mouth reference, joined with
the model's own mask, largest blob). Every 4th frame of each pose is held out; the new head must
see more tongue frames, produce no more false tongues and keep left and right apart. The personal
head is stored in `/data/local/tmp/tongue-head.bin` with a checksum.

## Verification done

- C++ port vs PyTorch on all 168 reference frames, on the headset: max logit difference 1e-3,
  0 differing mask cells, 0 differing presence/direction decisions (also after the retraining
  refactor).
- On-device training gradients vs PyTorch autograd on a real frame: relative difference <= 1e-5
  on every layer. Full retraining run on the author's recordings (353 frames, ~85 s): correctly
  rejected (the standard model already scores 1.0); the accept path saves and reloads a
  bit-identical head and derives a matching calibration.
- Model checked with whole recording sessions held out (never frame splits), see
  `research/tongue-v2/README.md`.
- Owner tests in VRChat: left/right/center, smile with teeth, pucker and its release, wide-open
  mouth, open mouth with the lower lip pushed out, closed mouth, reboot without ADB, calibration
  app runs, frame-by-frame check of a 20 s mirror recording.

## Offline retraining tools

`research/tongue-v2/training/` holds the PC pipeline the shipped model came from:
`shadow_model.py`, `train_shadow.py` / `train_shadow_v2.py`, `build_extra.py`,
`export_headset_model.py`. The author's training frames are attached to the release as
`tongue-training-data.zip`.
