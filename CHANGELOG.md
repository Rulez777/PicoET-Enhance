# Changelog

English | [简体中文](CHANGELOG_zh.md)

## [2.4.2-tongue.1] - 2026-10-27 (fork Rulez777/PicoET-Enhance)

### Added
- Tongue left/right: on-headset MobileNetV3-small segmentation model (C++ port, big cores 4-7, ~20 Hz), TongueLeft/TongueRight plus a TongueOut fallback, delivered to UnifiedPicoModule over Wi-Fi (UDP 19031, the PC subscribes; no ADB). On by default via `/data/local/tmp/tongue-live.ip`. See `TONGUE.md`.
- `sepolicy.rule`: UDP socket rules for the tongue channel (untested on an enforcing headset).
- Optional shadow mode (`/data/local/tmp/tongue-shadow.arm`) saves 200x200 mouth frames for retraining.

## [2.4.2] - 2026-10-24

### Fixed
- Pass the installed NDK 26.1.10909125 and CMake 3.22.1 paths explicitly as `-Ndk` and `-CmakeBin` to `build-hook.ps1`, fixing CI's selection of the runner's preinstalled NDK 27.3 / CMake 4.1.2.

### Design Rationale
- Installing a toolchain does not ensure the build selects it, so CI must explicitly select the same versions used for validation.

### Notes & Caveats
- `versionCode` is 20; runtime sources, default 90/90 targets, configuration, schema 2 / `EPI1` v2 / `X` v2 and companion requirements are unchanged.

## [2.4.1] - 2026-10-24

### Features
- Defer automatic injection until `sys.boot_completed=1` and Binder readiness from the same service instance. Early boot no longer performs injection or runtime-state cleanup.
- Bound automatic injection across consecutive unstable instances: at most three instances, replenished only after the same instance stays continuously `ready` for 60 seconds. Repeated control/watchdog initialization does not reset the count.

### Fixed
- Use `PTRACE_SEIZE` / `PTRACE_INTERRUPT` to stop and rescan all threads, verifying process/thread identity and mappings before remote execution. Read back restored GPR, FPSIMD and temporary instruction bytes before resuming; unsafe execution or restoration failures terminate the affected instance.
- Treat temporary signal stops and lack of a safe executor as injector exit `3`, not a permanent injection block. Release the control lock before the watchdog's 30-second cooldown, and return the unused automatic-injection reservation.
- Apply the existing single recovery budget to synchronous and asynchronous unsafe failures. Persist the reservation before acknowledging the failure, and avoid restarting a replacement instance that is already running.
- Serialize installation and runtime control with the same lock. Preserve saved `off` from legacy root-owned `0666` files during migration; use checked file descriptors and atomic replacement for configuration and runtime reports.
- Fix ByteSig's default-signal handling: recognize `SIG_DFL` and `SIG_IGN` before `SA_SIGINFO`, restore the default disposition and re-deliver to the same thread without changing the interrupted signal mask. Protected operations and custom handlers retain their existing behavior.
- Replace recursive uninstall cleanup with deletion of known leaf files and removal of empty directories, without traversing nested mounts. Unexpected directories may remain.

### Design Rationale
- Keep unsafe recovery separate from the unstable-instance limit: temporary preparation failures (injector exit `3`) consume neither budget, while early consecutive replacements without asynchronous failure reports remain subject to the unstable-instance limit. A new boot or explicit mode command resets the budgets.
- Keep engines initialized before hook installation running. Profile 0 and startup ET configuration are not applied retroactively; enhancement state warms up again without forcing an engine rebuild. `ready` describes installed hooks and configured targets, not reconstruction of an existing engine.
- Leave expression algorithms, inference models, default 90/90 targets and stock queue behavior unchanged. PICO system libraries remain unmodified.

### Notes & Caveats
- VersionCode is 19. Configuration paths, runtime schema 2, `EPI1` v2 / `X` v2 and companion properties are unchanged; PicoFacialBridge 0.5.0+ and UnifiedPicoModule 0.4.0+ remain the matching components. Install the scripts, injector and payload together, then reboot.
- The one-time frequency migration still applies only to installed versionCode below 18 (before 2.4.0). Reinstalling or upgrading from 2.4.0 or later preserves user-selected rates and saved `off`.
- Passed 188 offline checks (59 packaging/source, 123 shell and six existing logic-model checks), 35 injector control cases including targeted diagnostic fault checks, 20 payload file-I/O checks, four isolated uninstall checks, 19 ByteSig signal modes and 37 local callback/registry tests. The callback/registry tests do not run SNPE.
- On PICO 4 Pro / PICO OS 5.13.7 with SELinux permissive, normal boot, upgrade, off→dual and repeated ensure passed. A 45-second worn session confirmed live eye/mouth updates and action response, profile 0 initialization without retries, and normal STOP/END without a service restart. This was not a new 90 Hz throughput measurement; earlier throughput figures retain their documented scope. SELinux enforcing remains untested.

## [2.4.0] - 2026-10-18

### Features
- Eye and mouth expressions now run at 90/90 Hz by default, up from 90/24. Both paths process every eligible fresh camera input, with the two inferences for a frame running in parallel.
- Add rate files for lowering either rate: `/data/local/tmp/picoet-eye-hz` and `/data/local/tmp/picoet-mouth-hz` take a positive integer, are capped at 90, and fall back to 90 when missing or invalid. The mouth rate cannot exceed the eye rate, and frames outside the chosen rate are skipped. Setting only the mouth file to 24 gives 90/24; see the README.
- Rebuild mouth postprocessing from the raw 35-value mouth output into the 33 mouth slots. Smoothing, gated baseline adaptation and quality timing now advance by the mouth's own source time, as the eye path already did.
- `picoet.sh status` reports `effective_eye_hz` and `effective_mouth_hz`, the effective configured target rates, not measured output rates.

### Fixed
- Fix `picoet.sh dual` reporting success when the restarted eye-tracking service exited before enhancement was loaded, leaving `status` showing it unloaded and off. The command now reports the failure, and the background watchdog retries on its next pass. This also affected 2.3.0.

### Design Rationale
- Mouth smoothing involves a double k6 step whose second pass depends on the first pass's error, so the whole step is normalized to source time once rather than scaling each pass separately. When the time ratio is one, the stock floating-point order is kept: over a 2,700-frame control, all 52 float values, validity bits and source identities match the reference output exactly.
- The inference models, same-frame parallelism and stock queue-full behavior are unchanged, and rates are never lowered automatically under load. The stock 24 Hz reference period also stays as it was.

### Notes & Caveats
- The rate files are read once when the service starts. Apply a change by running `picoet.sh dual`, which restarts the service and disconnects tracking clients; the command refuses while clients are connected unless you add `--force`. Normal mode (`off`) stays fully stock.
- Fresh installs enable `dual/on` at 90/90, and an upgrade keeps a saved `off`. Installing 2.4.0 over an earlier version starts at 90/90 once; rates you set afterwards are kept when you reinstall or upgrade.
- Deploy the payload and the matching `picoet.sh` together, since the runtime state now carries both rates (schema 2).
- Mouth baseline adaptation and quality timing are time-normalized approximations, so output at different rates is close but not bit-identical.
- No PicoFacialBridge or UnifiedPicoModule changes: `EPI1` v2 and `X` v2 are unchanged, and PicoFacialBridge 0.5.0+ with UnifiedPicoModule 0.4.0+ remain the matching versions.
- Measured on an awake, tracking-ready PICO 4 Pro (not worn) by replaying 2,700 recorded camera inputs at 90/90 through a test harness that drives the module's expression-processing algorithm: eye and mouth computation completed at 89.80 Hz, with six inputs dropped by the stock full-queue rule and no inference failures. With VRCFT-Enhance and desktop VRChat, 2,633 sources (87.80 Hz) carried eye and mouth together through `X`, SDK IPC and OSC with values matching at each stage; the loopback packet capture dropped packets, so the downstream counts are verifiable lower bounds. The replay was not run through the installed Magisk package and does not cover live camera copying or Virtual Desktop decoding and rendering. See the README for details.

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
- The official VRCFT host passes this stream to OSC at roughly 61–78 Hz on the test PC. The optional [VRCFT-Enhance](https://github.com/WolalaQAQ/VRCFT-Enhance) raises it to about 87–88 Hz; see the README.

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
