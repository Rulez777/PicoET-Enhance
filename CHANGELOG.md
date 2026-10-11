# Changelog

English | [简体中文](CHANGELOG_zh.md)

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
