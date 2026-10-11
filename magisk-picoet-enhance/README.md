# PicoET-Enhance

Eye-tracking enhancement for PICO 4 Pro running PICO OS 5.13.7 with Magisk.
The installer checks the device's eye-tracking library hashes before installing.

## Installation and defaults

Install the ZIP in Magisk and reboot. Fresh installs enable enhanced mode (`dual/on`).
Selecting normal mode keeps enhancement disabled across subsequent reboots.

Dual updates each eye independently. Tracking loss in one eye leaves the other
updating, while combined gaze retains the stock processing. Use PicoFacialBridge
to send the data to UnifiedPicoModule 0.2.0 or later on the PC, where gaze smoothing
is configured.

## Mode controls

Run as root:

```sh
sh /data/adb/modules/picoet-enhance/picoet.sh status
sh /data/adb/modules/picoet-enhance/picoet.sh dual
sh /data/adb/modules/picoet-enhance/picoet.sh off
sh /data/adb/modules/picoet-enhance/picoet.sh toggle
```

`dual` enables enhancement, `off` restores the stock service, and `toggle` switches
between them. The Magisk Action button performs the same toggle and retries
enabling if enhancement previously failed.

Switching restarts the eye-tracking service and disconnects tracking clients.
Command-line switching requires `--force` while clients are connected.

## Status and troubleshooting

Successful enhancement shows `phase=ready applied_mode=dual applied_gate=on` in
`status`. If loading stops, read the reported reason and logs, then run `dual` again
after resolving it. Automatic recovery allows one retry per boot or mode command.

- Hook log: `/data/local/tmp/picoet-hook.log`
- Control logs: `/data/adb/picoet-enhance/*.log`
- Saved mode/gate: `/data/local/tmp/picoet-mode`, `/data/local/tmp/picoet-gate`

To remove the module, uninstall it in Magisk and reboot.

See the [project README](https://github.com/WolalaQAQ/PicoET-Enhance#compatibility-and-verification)
for compatibility and validation details. The package contains the module scripts,
injector and hook payload; device libraries are supplied by the headset firmware.
