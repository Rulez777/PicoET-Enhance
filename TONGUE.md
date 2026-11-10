# Tongue left/right for PICO 4 Pro / Enterprise (PicoET-Enhance + UnifiedPicoModule)

PICO's own face tracking reports only `TongueOut`. This adds **TongueLeft / TongueRight** (and a
model-based TongueOut fallback) from the mouth camera, computed **on the headset**, delivered to
VRCFaceTracking over Wi-Fi. No ADB, no PC-side image processing, no firewall rules.

> The shipped network was trained on one person's face. A different face or headset fit can read
> the direction wrongly, or take an upper lip or a beard for a tongue: run the **Tongue
> Calibration** app (about a minute). If the tongue is still not seen, its **Retrain** option
> teaches the model on your face, on the headset itself.

## Install (two files, nothing else)

You need a rooted (Magisk) PICO 4 Pro or PICO 4 Enterprise and a PC with VRCFaceTracking 5 or 6
(the Steam VRCFT 6 build included). Both files are in the
[latest release](https://github.com/Rulez777/PicoET-Enhance/releases/latest).

1. **Headset:** Magisk app → Modules → Install from storage →
   `magisk-picoet-enhance-v2.4.2-tongue.4.zip` → Reboot. This one file installs everything the
   headset needs: the module, the **Tongue Calibration** app and **PicoFacialBridge v0.6.0** (with
   its tracking permissions granted). An older Bridge is upgraded, a newer one is left alone.
2. **PC:** unpack `UnifiedPicoModule-0.4.1-tongue.4.zip`, close VRCFaceTracking, double-click
   `Install.cmd`, start VRCFaceTracking. The script copies the module into
   `%APPDATA%\VRCFaceTracking\CustomLibs\8322e16d-...`, keeps your `UnifiedPicoModule.json` and
   saves the previous DLL as `UnifiedPicoModule.dll.previous`. The module finds the headset the
   same way as PicoFacialBridge (auto-discovery, or `Bridge.IP` if you set one). If your face comes
   through PICO Connect streaming instead, set `Bridge.IP` to the headset's IP.
3. **Once:** put the headset on, start PicoFacialBridge, then run **Tongue Calibration**.

Avatars need TongueLeft/TongueRight (Unified Expressions).
Tongue is on by default; to switch it off delete `/data/local/tmp/tongue-live.ip` (root).

Nothing is tied to one particular headset: there are no serial numbers, IP addresses, CPU core
numbers or fixed image sizes in the code. The fast cores are found from cpufreq, the mouth image
is scaled from whatever size the camera delivers, the nose reference adapts to the glare the
head-mount gives, and all thresholds in the app are relative to the wearer's own recordings.

### Firmware

The module installs on any firmware. The eye/face enhancement of PicoET-Enhance (independent
eyes, 90 Hz expressions) patches PICO's tracking plugin at exact addresses, so it runs only on the
build it was verified against, PICO OS 5.13.7. On any other firmware:

- the tracking plugin is left stock;
- the tongue still runs: the function that receives the mouth image is found by its code (72
  instructions of the verified build with every address and offset masked, up to 20 of them may
  differ, exactly one place must match) and is only observed, never changed
  (`hook/src/tongue_compat.h`);
- the per-eye gate (real pupil size) is hooked by its exported symbol name, as before.

If the function is not recognised, nothing is hooked and `/data/local/tmp/picoet-hook.log` says
`tongue-only: FaceTrackor not recognised`. This mode was exercised on 5.13.7 only (there it finds
the same function the address-based hook uses); other firmware builds are untested.
To use this mode on 5.13.7 too (tongue and pupil size, stock expressions), create the root-owned
file `/data/local/tmp/picoet-tongue-only` and run `picoet.sh dual --force`.

### Diagnostics are off

Release builds write no per-frame log and keep no camera frames. The research capture and the
`tongue-shadow.log` / `tongue-shadow.frames` files exist only in builds configured with
`-DPICOET_TONGUE_DIAG=ON`. On the PC, `tongue-status.json` and the once-a-second log line appear
only when an empty file `tongue-diagnostics.enabled` is placed next to the DLL. The calibration
app shows the live camera picture and mask, which is the normal way to see what the model sees.

If the Bridge on the headset was signed with a different key than the official release (a
self-built debug APK), Android cannot upgrade it in place: the module removes it and installs the
official one, which resets the Bridge's settings.

## Calibration app

Open **Tongue Calibration** with the headset on:

- **Calibrate** (recommended, about a minute): headset fit check, then five poses: closed mouth,
  open mouth without tongue, tongue straight out, to your left and slightly down, to your right
  and slightly down. Every pose has the same three phases, so there is time to read: **get
  ready** (3 second countdown, grey ring, nothing is recorded), **recording** (green ring fills
  while you hold the pose), **Done!** with the next instruction. It saves your tongue angles and a
  size threshold between your no-tongue mouth and your tongue, then shows a live left/right test.
- **Ignored zone** (part of Calibrate): whatever the model marks as tongue while your mouth is
  closed or open *without* a tongue (a brightly lit upper lip, a moustache or beard edge) is
  remembered as a per-user map of ignored cells. Those cells never count as tongue afterwards. The
  home screen shows `Ignored zone: N` when the map is not empty. Not yet tested on a real beard:
  reports are welcome.
- **Retrain (advanced)**, only if calibration did not help (tongue not seen on one side, or false
  tongues): records the same poses for about 15 seconds, then trains the model's mask head on the
  headset for 1-2 minutes. The new model is kept only if it tests better than the current one on
  held-out frames; otherwise nothing changes. A poor recording can make tracking worse, so the
  app warns first.
- **Restore standard** brings back the shipped model and calibration and clears the ignored zone
  at any time.

After re-seating the headset differently, a quick re-calibration may help: the tongue's angle in
the camera depends on how the headset sits.

## How it works

```
mouth camera (8-bit grey, 400x400 on PICO 4 Pro / Enterprise, inside pxreyetrackingservice)
  -> payload copies a box-averaged 200x200 frame at <=20 Hz (tracking thread does nothing else)
  -> worker thread on the fastest cores, found from cpufreq (off the tracking service's little
     cores)
       MobileNetV3-small blocks 0..8 + small mask head, float32 C++ port, ~30 ms/frame
       ignored zone: cells of the per-user map (/data/local/tmp/tongue-suppress.bin) are dropped
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
