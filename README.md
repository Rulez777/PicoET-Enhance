# PicoET-Enhance

English | [简体中文](README_zh.md)

An eye-tracking enhancement Magisk module for the PICO 4 Pro: it opens the eye-tracking service's per-eye gate for real pupil data, decouples the facial expression model so each eye is processed on its own, and runs eye expressions at the camera's 90 Hz instead of the stock rate of about 24 Hz. Use it with [PicoFacialBridge](https://github.com/WolalaQAQ/PicoFacialBridge) and [UnifiedPicoModule](https://github.com/WolalaQAQ/UnifiedPicoModule).

> v2.3.0 · PICO 4 Pro / PICO OS 5.13.7 · [Changelog](CHANGELOG.md)

## What it does

The PICO 4 Pro firmware tracks each eye, but its facial expression model mixes the two eyes before output, so blink, look direction, wideness and brows for one eye depend on the other eye's inputs. It also updates these expressions only about 24 times a second, although the eye cameras run at 90 Hz. This module changes three things at runtime:

- **Real pupil data**: it opens the per-eye gate inside the eye-tracking service, so real pupil diameter data is written to shared memory.
- **Independent eyes**: it removes the expression model's cross-eye coupling. EyeLook, Blink, Wide and the brow channels are computed and filtered per eye, each with its own quality window, reference image and history. The filtered right inner brow and the per-eye quality states are exported as an `EPI1` extension, which PicoFacialBridge forwards as `X` packets.
- **90 Hz eye expressions**: the complete eye-expression path runs on every new camera frame, so eye values reach the PC at about 90 Hz. In an 84-second worn session through Virtual Desktop, about 89.7 `X` packets per second arrived at the PC, each carrying a new eye source. The stock mouth chain stays at about 24 Hz, and eye and mouth inference run in parallel on the frames they share. To carry this rate all the way to VRChat, see [About 90 Hz OSC](#about-90-hz-osc).

Eye smoothing uses each eye's source-time interval, and quality windows pause across missing frames. Repeated or backward source timestamps do not advance that eye's state.

The combined gaze and the mouth algorithm stay stock. Each eye keeps blink endpoints, filtering and the within-eye inner/outer brow linkage. Dark-image admission, reference protection and model quantization remain enabled. On the PC, eye direction and eyelid openness use EyeLook and `1−Blink`; native eye data supplies measured pupil diameter only.

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

The high-rate path uses `EPI1` / `X` v2 and needs PicoFacialBridge 0.5.0 or later and UnifiedPicoModule 0.4.0 or later. The earlier PicoET-Enhance 2.2.0 with PicoFacialBridge 0.4.0 and UnifiedPicoModule 0.3.0 provides per-eye independence at the stock expression cadence using v1. Older Bridge versions (0.3 and up) still work in normal mode without this module.

### About 90 Hz OSC

The eye path delivers about 90 Hz to the PC, but the official VRCFaceTracking host passes it on to OSC at a lower rate: it reads module output and writes OSC on fixed 10 ms waits. We measured this on a PICO 4 Pro streaming through Virtual Desktop, replaying 2,700 recorded eye frames at their original 90 Hz timing and counting new eye directions at the OSC output:

| VRCFT host (5.4.5, Steam build `7add148`) | New eye directions per second |
|---|---:|
| Official, unmodified | about 61–78 Hz |
| Patched, 4 ms waits | about 87–88 Hz |

Windows 11 may ignore the 1 ms timer resolution the official host asks for. When it does, each 10 ms wait takes about 15 ms, which is why the official result varies from run to run.

These figures came from a Ryzen 7 9700X, RTX 4070 Ti SUPER, 64 GB RAM and Windows 11 Pro for Workstations on the Ultimate Performance power plan. Other PCs may see different numbers.

The module works with the official VRCFaceTracking as is. If you want OSC close to 90 Hz, use [osc90-host-patch](https://github.com/WolalaQAQ/osc90-host-patch). It has the host and module processes keep Windows from ignoring their timer resolution, turns the two fixed waits into settings (4 ms by default), and fixes a few unsynchronized data exchanges in the host that shorter waits make more likely to collide. It is a source patch: you build it yourself and run it from a separate copy of VRCFaceTracking. No VRCFaceTracking binaries are redistributed.

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
   adb push magisk-picoet-enhance-v2.3.0.zip /data/local/tmp/
   adb shell su -c 'magisk --install-module /data/local/tmp/magisk-picoet-enhance-v2.3.0.zip'
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

The high-rate path has been validated on PICO 4 Pro / PICO OS 5.13.7 with PicoFacialBridge 0.5.0, UnifiedPicoModule 0.4.0 and official VRCFT 5.4.5. In an 84-second worn session streaming through Virtual Desktop, with Cymple mouth tracking and PICO eyes, the Bridge sent about 89.7 `X` packets per second to the PC, each with a new eye source. PICO eyes with face tracking, without Cymple, also ran normally. Earlier recorded-input tests covered inference throughput and transport.

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
- [ ] Eye and face tracking in sync at 90 Hz: eye expressions already run at the camera rate, while the stock mouth chain stays at about 24 Hz. The goal is to bring the face model up to the same rate so eye and mouth values reach the PC, and then OSC, together at 90 Hz, with both models sharing the headset's compute budget during streaming.

## Feedback

Issues are welcome. Please include the headset model, PICO OS version, module version, the current mode, the output of `picoet.sh status`, and relevant log excerpts.

## License

The original code and scripts of this project are licensed under [MIT](LICENSE). The vendored ShadowHook 2.0.1 sources include MIT- and BSD-licensed dependencies; the Magisk ZIP includes their notices and the project license. This project contains and distributes no PICO binary; it only references the device's own libraries by numeric offset at runtime. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
