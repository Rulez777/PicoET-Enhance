# Changelog

English | [简体中文](CHANGELOG_zh.md)

## [2.3.0] - 2026-10-17

### Features
- Run the complete independent eye-expression path on fresh camera inputs while keeping the stock mouth chain at about 24 Hz, with same-frame eye/mouth inference in parallel.
- Export `EPI1` v2 with separate left-eye, right-eye and mouth source identities and update bits for `X` v2 companion builds.

### Changed
- Keep the toolchain's default GNU Build ID on the hook payload and helpers, and drop the build-time check that rejected it. The Build ID is a normal part of the link output and not related to the module's own integrity checks, which are unchanged.

### Design Rationale
- Normalize eye smoothing to each eye's source-time interval. Quality and reference decisions use each eye's effective observation time, so missing frames pause the window and repeated or backward sources do not advance it.
- Apply SNPE profile 0 only to the first eligible builder during guarded, uninitialized FaceTrackor eye/mouth initialization. Native eye tracking, runtime selection, priority and retry settings remain stock.

### Notes & Caveats
- Requires PicoFacialBridge 0.5.0 or later and UnifiedPicoModule 0.4.0 or later for the `X` v2 path; the earlier 0.4.0 / 0.3.0 pair supports the `X` v1 path only.
- Keep P1, boot loading, mode switching, dark-image admission, reference protection, within-eye rules, filtering and quantization. A partially failed owner remains sealed until normal lifecycle reconstruction.
- Validated on a PICO 4 Pro with PicoFacialBridge 0.5.0, UnifiedPicoModule 0.4.0 and official VRCFT 5.4.5 in an 84-second worn Virtual Desktop session with Cymple mouth tracking and PICO eyes: about 89.7 `X` packets per second reached the PC, each with a new eye source. PICO eyes with face tracking also ran normally. This was a short session, not a long-duration stability test.
- The official VRCFT host passes this stream to OSC at roughly 61–78 Hz on the test PC. The optional [osc90-host-patch](https://github.com/WolalaQAQ/osc90-host-patch) raises it to about 87–88 Hz; see the README.

## [2.2.0] - 2026-10-15

### Features
- Decouple the facial expression model: EyeLook, Blink, Wide and the brow channels are computed and filtered per eye, each with its own quality window, reference image, numerical history and Kalman filter.
- Open the service-local gate (`PXR::EyeUtil::isBusinessDev` -> true) so the eye-tracking service writes real pupil diameter data to shared memory.
- Export the filtered right inner brow and the per-eye quality states to the facial prefix as the `EPI1` extension, which PicoFacialBridge forwards as `X` packets.

### Changed
- Remove the native gaze modifications: the per-eye single-eye inference dispatch, the plugin-tail leaf and the left/right eye-mode patch. Combined gaze now remains fully stock.
- Accept `on` as a synonym for `dual` in `picoet.sh`.

### Fixed
- Fix SELinux policy loading on enforcing builds (#3): use Magisk statement syntax, allow traversal of `/data/adb` and the module tree, replace the nonexistent `mem` class with `/proc` metadata permissions, and name the service type explicitly for `execmem` instead of using `self`.
- Include the project license and third-party notices in the Magisk ZIP, with the xDL, linux-syscall-support and BSD header notices used by ShadowHook.

### Design Rationale
- The stock model mixes the two eyes before output, so one eye's closure, quality or history can change the other side. The independent path keeps each eye's own raw inference result, quality window, reference and history while the stock mouth processing and owner lifecycle keep running.
- Reuse the stock per-eye rules instead of approximating them: blink endpoints, the within-eye inner/outer brow linkage and the `correct`->`predict` Kalman order, instantiated once per eye inside the service.

### Notes & Caveats
- Validated on PICO 4 Pro / PICO OS 5.13.7 with SELinux permissive: recorded inputs reproduce the independent output and match the stock baseline on all non-eye coefficients and the mouth validity. Recorded eye images replayed on the headset through native eye inference reached official VRCFT 5.4.5 via PicoFacialBridge 0.4.0 and UnifiedPicoModule 0.3.0 with `E` and `X` values matching their source outputs. The recording is about 23.7 Hz and was replayed in an accelerated loop, so this verifies inference throughput and transport, not an original 90 Hz camera sequence.
- Requires PicoFacialBridge 0.4.0 or later and UnifiedPicoModule 0.3.0 or later for the per-eye `X` path. Single-eye quality failure (classification 0), staggered reference rebuild, a live mouth-output comparison and long live-camera use are not yet covered.
- PICO 4 Enterprise with SELinux enforcing remains unverified; the policy update requires a module reinstall and reboot.
- The extension carries both inner-brow sides, but the host-side brow parameters used by current avatars average them. The original within-eye inner-to-outer brow write-back is intentionally kept.
- The build now fails if a payload or helper contains a GNU Build ID.

## [2.1.0] - 2026-10-11

### Fixed
- Make dual output independent per eye and remove the shared fused-filter residual. Failure clears only that eye's gaze-valid bit while the other keeps updating.
- Store per-frame results in thread-local state and preserve caller registers and floating-point state in Hook B. Combined gaze retains the original processing.
- Fix enhancement failing to re-enable when Magisk launches scripts with relative paths.

### Changed
- Use two control modes: enhanced (`dual/on`) and normal (`off/off`). Remove the `left`, `right` and separate `gate` commands.
- Make the Magisk Action button switch according to actual readiness and retry enabling after failed loading.
- At upgrade and boot, normalize a saved `off` to `off/off` and other previous settings to `dual/on`.
- Strip ELF debug information before packaging.

## [2.0.4] - 2026-10-08

### Changed
- Default fresh installs to `dual` + `gate on`, automatically enabling genuine per-eye gaze and pupil data after reboot without manual mode selection. Installation messages show the saved state.
- Preserve valid mode/gate settings on upgrade, including `off` and `gate off`. Runtime failures still advertise only successfully installed hooks, never a forced enhanced state.

### Validation
- Android NDK builds, hook shape checks, shell/PowerShell syntax checks, module packaging and public-content checks pass.
- The new installation defaults have not yet been validated on a headset.

## [2.0.3] - 2026-10-03

### Fixed
- Save and restore the executor's complete general-purpose and FPSIMD context during injection. Refuse remote calls if the context cannot be saved.
- Verify the remote-call return trap. Timeouts, unexpected signals and failed restoration terminate the affected service instead of resuming a half-finished loader call. Automatic recovery allows one fresh-process retry per boot or explicit mode/gate command; subsequent failures stop automatic injection.
- Disable ShadowHook's ELF-gap allocation fallback in this integration so it cannot overwrite the hand-written Hook B leaf in the plugin tail.
- Publish the hooks actually installed, using atomic runtime reports bound to the service PID and start time. Pending or failed plugin installation no longer advertises independent dual gaze.
- Reconcile watchdog state every second, including asynchronous plugin completion, service replacement and read-only mapping fragments. Serialize watchdog and manual control commands.
- Stop all plugin installation after service compatibility or hook initialization fails, and require plugin guards before applying left/right patches. Explicit `gate off` remains supported.

### Changed
- Extend `status` with installation phase, applied mode/gate and automatic-recovery stop reasons.
- Preserve the running instance's advertised state when staging an upgrade; clear runtime reports at boot and uninstall.

### Validation
- Android NDK builds, hook shape checks, shell/PowerShell syntax checks and module packaging pass.
- This release's register restoration, failure recovery and runtime-state permissions have not yet been revalidated on a headset. The supported firmware remains PICO OS 5.13.7 on PICO 4 Pro.

## [2.0.2] - 2026-10-02

### Added
- Initial public runtime-injection release with a service-local per-eye gate and off, left, right and dual gaze modes.
- Magisk lifecycle scripts, transient companion-app mode properties, firmware guards and source-only hook builds without bundled PICO libraries.
