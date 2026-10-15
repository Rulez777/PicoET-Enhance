# PicoET-Enhance

English | [简体中文](README_zh.md)

An eye-tracking enhancement Magisk module for the PICO 4 Pro: it opens the eye-tracking service's per-eye gate for real pupil data and decouples the facial expression model so each eye is processed on its own, for use with [PicoFacialBridge](https://github.com/WolalaQAQ/PicoFacialBridge) and [UnifiedPicoModule](https://github.com/WolalaQAQ/UnifiedPicoModule).

> v2.2.0 · PICO 4 Pro / PICO OS 5.13.7 · [Changelog](CHANGELOG.md)

## What it does

The PICO 4 Pro firmware tracks each eye, but its facial expression model mixes the two eyes before output: blink, look direction, wideness and brows for one eye depend on the other eye's inputs. This module corrects that at runtime in two ways:

- it opens the per-eye gate inside the eye-tracking service, so real pupil diameter data is written to shared memory;
- it removes the expression model's cross-eye coupling: EyeLook, Blink, Wide and the brow channels are computed and filtered per eye, each with its own quality window, reference image and history. The filtered right inner brow and the per-eye quality states are exported as an `EPI1` extension, which PicoFacialBridge forwards as `X` packets.

The stock mouth processing and the combined gaze stay as they are. Each eye keeps the original per-eye rules: blink endpoints, smoothing and the within-eye inner/outer brow linkage.

The enhancement runs as hooks in the eye-tracking service and is unloaded when that service restarts.

## How it fits together

```text
PicoET-Enhance (headset, this module)
        ↓ active at runtime, advertises the mode
PicoFacialBridge (headset)
        ↓ BridgeSplit, LAN UDP 9030
UnifiedPicoModule (PC)
        ↓
VRCFaceTracking → VRChat OSC
```

PicoFacialBridge reads the module's transient mode properties and automatically tells the PC whether normal or enhanced mode is active.

Per-eye independent expressions need PicoET-Enhance 2.2.0 or later on the headset, PicoFacialBridge 0.4.0 or later, and UnifiedPicoModule 0.3.0 or later on the PC. Older Bridge versions (0.3 and up) still work in normal mode without this module.

## Requirements

| Item | Requirement |
|---|---|
| Headset | PICO 4 Pro |
| Firmware | PICO OS 5.13.7 (Android 10 / API 29, arm64-v8a) |
| Root | Magisk installed |
| Companion components | PicoFacialBridge and UnifiedPicoModule, to feed the data into VRCFaceTracking |

The installer verifies the SHA-256 of the device's eye-tracking libraries and stops if the firmware build does not match.

## Installation

1. Download `magisk-picoet-enhance-v<version>.zip` from [Releases](https://github.com/WolalaQAQ/PicoET-Enhance/releases).
2. Flash it in the Magisk app under Install from storage, or from a command line:

   ```sh
   adb push magisk-picoet-enhance-v2.2.0.zip /data/local/tmp/
   adb shell su -c 'magisk --install-module /data/local/tmp/magisk-picoet-enhance-v2.2.0.zip'
   ```

3. Reboot the headset.

Fresh installs enable enhanced mode (`dual/on`) after reboot. Selecting normal mode keeps enhancement disabled across subsequent reboots.

## Usage

Run as root on the headset:

```sh
sh /data/adb/modules/picoet-enhance/picoet.sh status
sh /data/adb/modules/picoet-enhance/picoet.sh dual
sh /data/adb/modules/picoet-enhance/picoet.sh off
sh /data/adb/modules/picoet-enhance/picoet.sh toggle
```

| Command | Effect |
|---|---|
| `status` | Shows the saved mode, injection state, service PID and the current `ro.pxr.externalfunc` value |
| `dual` | Enhanced mode: sets `dual/on` together, opening the pupil gate and enabling the per-eye independent expressions |
| `off` | Normal mode: sets `off/off` together and restarts the service without injection; fully stock |
| `toggle` | Switches to normal mode if enhancement is ready; otherwise attempts to enable enhancement |

Switching restarts the eye-tracking service and disconnects eye and face tracking clients; command-line switching requires `--force` while clients are connected. The Magisk app's Action button switches directly between enhanced and normal mode. After an enhancement failure, pressing it again retries enabling.

`on` is accepted as a synonym for `dual`. The state files are `/data/local/tmp/picoet-mode` and `/data/local/tmp/picoet-gate`, and the log is `/data/local/tmp/picoet-hook.log`.

## About the enterprise gate

The firmware uses `PXR::EyeUtil::isBusinessDev()` to control per-eye output, including the real pupil diameter. This module makes the function return true inside the eye-tracking service. The device-wide enterprise-mode property `ro.pxr.externalfunc` keeps its existing value.

## Compatibility and verification

The per-eye expression path has been validated on PICO 4 Pro / PICO OS 5.13.7. Recorded inputs reproduce the independent output and match the stock baseline on all non-eye coefficients and the mouth validity. End to end, eye images recorded earlier and replayed on the headset through native eye inference reached official VRCFT 5.4.5 via PicoFacialBridge 0.4.0 and UnifiedPicoModule 0.3.0, and the `E` and `X` values received matched the source outputs. The recording is about 23.7 Hz and was replayed in an accelerated loop, so this shows inference throughput and transport rather than a real 90 Hz camera sequence.

Not yet covered: single-eye quality failure, staggered reference rebuild, a live mouth-output comparison and long live-camera sessions. The stock shared mouth processing and combined gaze remain unchanged, and the host-side brow parameters used by current avatars average the two sides even though the extension carries both.

## Disabling and uninstalling

Run `picoet.sh off` to disable enhancement and restore the stock eye-tracking service. To remove the module, uninstall it in Magisk and reboot the headset.

## Building from source

Building requires the Android NDK and CMake/Ninja. The scripts look for the toolchain in the `-Ndk` / `-CmakeBin` parameters, the `PICOET_NDK` / `PICOET_CMAKE_BIN` environment variables, or a standard layout under `ANDROID_HOME`.

```powershell
pwsh -File hook\build-hook.ps1                 # build the injector and payload
pwsh -File build-picoet-enhance.ps1            # package the Magisk zip
```

The output is `artifacts/magisk-picoet-enhance-v<version>.zip`, containing the module scripts, injector and hook payload. Packaging checks the ELF file list and stock-library hashes.

Pushing a `v<version>` tag makes GitHub Actions build on a Windows runner and publish the zip with a checksum.

## TODO

- [ ] Tongue-direction support: PICO currently exposes only the scalar `TongueOut`, while VRCFaceTracking already defines direction channels such as `TongueLeft/Right/Up/Down`. The plan is to capture raw frames from the lower-face camera, auto-label them with visual segmentation foundation models, train a small model, and let users collect their own data for a local online fine-tune.
- [ ] Unfiltered face output: make the on-device FaceTrackor temporal smoothing optional so the module can forward per-frame raw face data. Filtering then moves to the PC side, where it can be tuned as needed.

## Feedback

Issues are welcome. Please include the headset model, PICO OS version, module version, the current mode, the output of `picoet.sh status`, and relevant log excerpts.

## License

The original code and scripts of this project are licensed under [MIT](LICENSE). The vendored ShadowHook 2.0.1 sources include MIT- and BSD-licensed dependencies; the Magisk ZIP includes their notices and the project license. This project contains and distributes no PICO binary; it only references the device's own libraries by numeric offset at runtime. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
