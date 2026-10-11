# PicoET-Enhance

English | [简体中文](README_zh.md)

An eye-tracking enhancement Magisk module for the PICO 4 Pro: it makes the headset's eye-tracking service output true per-eye gaze and pupil data, for use with [PicoFacialBridge](https://github.com/WolalaQAQ/PicoFacialBridge) and [UnifiedPicoModule](https://github.com/WolalaQAQ/UnifiedPicoModule).

> v2.1.0 · PICO 4 Pro / PICO OS 5.13.7 · [Changelog](CHANGELOG.md)

## What it does

The PICO 4 Pro firmware already computes gaze for each eye, but by default it only exposes the fused result. This module corrects that at runtime in two ways:

- it opens the per-eye gate inside the eye-tracking service, so per-eye gaze and real pupil data are written to shared memory;
- it makes the gaze algorithm run the left and right single-eye models so the per-eye vectors are genuine (`dual` mode).

In dual mode, each eye outputs its own single-eye inference result. If tracking is lost in one eye, the other continues updating and the PC module holds the missing eye's last valid value. The combined gaze used by applications such as OpenXR retains the original inference and filtering.

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

UnifiedPicoModule 0.2.0 or later is recommended. It enables mode-specific adaptive smoothing by default; see its [gaze smoothing configuration](https://github.com/WolalaQAQ/UnifiedPicoModule/blob/main/docs/configuration.md#gaze-smoothing) for the parameters.

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
   adb push magisk-picoet-enhance-v2.1.0.zip /data/local/tmp/
   adb shell su -c 'magisk --install-module /data/local/tmp/magisk-picoet-enhance-v2.1.0.zip'
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
| `dual` | Enhanced mode: sets `dual/on` together, enabling genuine per-eye gaze and pupil data |
| `off` | Normal mode: sets `off/off` together and restarts the service without injection; fully stock |
| `toggle` | Switches to normal mode if enhancement is ready; otherwise attempts to enable enhancement |

Switching restarts the eye-tracking service and disconnects eye and face tracking clients; command-line switching requires `--force` while clients are connected. The Magisk app's Action button switches directly between enhanced and normal mode. After an enhancement failure, pressing it again retries enabling.

The state files are `/data/local/tmp/picoet-mode` and `/data/local/tmp/picoet-gate`, and the log is `/data/local/tmp/picoet-hook.log`.

## About the enterprise gate

The firmware uses `PXR::EyeUtil::isBusinessDev()` to control per-eye output. This module makes the function return true inside the eye-tracking service. The device-wide enterprise-mode property `ro.pxr.externalfunc` keeps its existing value.

## Compatibility and verification

Independent output has been observed on PICO 4 Pro / PICO OS 5.13.7 / SELinux permissive: the usable eye keeps updating during single-eye loss, and VD 1.34.22.0 foveated-region following and eye-menu pointing work normally. The new control script's two-mode switching, relative-path launch and enforcing SELinux rules await headset validation.

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

The original code and scripts of this project are licensed under [MIT](LICENSE). `hook/third_party/shadowhook-2.0.1/` is a source copy of ByteDance ShadowHook 2.0.1, licensed under MIT. This project contains and distributes no PICO binary; it only references the device's own libraries by numeric offset at runtime. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
