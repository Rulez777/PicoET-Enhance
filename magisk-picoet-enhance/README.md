# PicoET-Enhance 2.4.2

Eye-tracking enhancement for PICO 4 Pro running PICO OS 5.13.7 with Magisk.
The installer checks the device's eye-tracking library hashes before installing.

## Installation and defaults

Install the ZIP in Magisk and reboot. Fresh installs enable enhanced mode (`dual/on`)
at 90/90 Hz. If you switched to normal mode before upgrading, it stays off.

Automatic injection waits for `sys.boot_completed=1` and a Binder response from the
same eye-tracking service instance. Before starting Bridge or another tracking client
in enhanced mode, check that `picoet.sh status` reports
`phase=ready applied_mode=dual applied_gate=on`.

Enhanced mode opens the service-local pupil gate and decouples the expression model:
each eye's EyeLook, Blink, Wide and brow values are computed and filtered on their own,
and the right inner brow plus the per-eye quality states are exported as the `EPI1`
v2 extension. PicoFacialBridge forwards it as `X` v2 packets with separate left-eye,
right-eye and mouth source identities. Use matching Bridge and UnifiedPicoModule
builds with v2 support: PicoFacialBridge 0.5.0+ and UnifiedPicoModule 0.4.0+.

Eye and mouth expressions each run on every eligible fresh camera input, with the two
inferences for a frame in parallel. Smoothing, mouth baseline adaptation and quality
windows follow each source's own timestamps. Dark-image admission, reference
protection, quantization and combined native gaze remain stock. PC eye direction and
eyelid openness use EyeLook and `1−Blink`; native eye data is used only for measured
pupil diameter.

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

## Eye and mouth rates

The defaults are 90 Hz for eyes and 90 Hz for mouth, with no configuration needed.
To lower either one, write a positive integer to `/data/local/tmp/picoet-eye-hz` or
`/data/local/tmp/picoet-mouth-hz`. Values above 90 are capped, a missing or invalid
file uses 90, and the mouth rate cannot exceed the eye rate. Frames outside the chosen
rate are skipped. The files are read once when the service starts, so stop tracking
clients and re-run `dual` to apply a change. For example, to run the mouth at 24 Hz:

```sh
echo 90 > /data/local/tmp/picoet-eye-hz
echo 24 > /data/local/tmp/picoet-mouth-hz
sh /data/adb/modules/picoet-enhance/picoet.sh dual
sh /data/adb/modules/picoet-enhance/picoet.sh status
```

`status` shows `effective_eye_hz` and `effective_mouth_hz`, the rates in effect rather
than measured output rates. To return to 90/90, delete both files and run `dual`
again. Rates set in 2.4.0 or later survive reinstalls and upgrades. Only the first
migration from a version before 2.4.0 clears old overrides to start at 90/90.
Normal mode (`off`) is fully stock.

## Status and troubleshooting

Successful enhancement shows `phase=ready applied_mode=dual applied_gate=on` in
`status`. This describes installed hooks and configured targets. Engines initialized
before the hooks are not rebuilt or retroactively given profile 0 or startup ET
settings; the enhancement's own state warms up again.

No safe executor or a temporary signal stop during preparation returns injector exit
`3`: the watchdog releases the control lock and cools down for 30 seconds without
consuming either budget. Synchronous and asynchronous unsafe failures share one
automatic recovery per boot or explicit mode command. A separate limit allows
injection into at most three consecutive unstable instances, replenished only after
the same instance stays continuously `ready` for 60 seconds. If automatic loading
stops, read the reported reason and logs, resolve it, then run `dual` to retry.

- Hook log: `/data/local/tmp/picoet-hook.log`
- Control logs: `/data/adb/picoet-enhance/*.log`
- Saved mode/gate: `/data/local/tmp/picoet-mode`, `/data/local/tmp/picoet-gate`

To remove the module, uninstall it in Magisk and reboot. Cleanup removes known leaf
files without recursing into unexpected directories or nested mounts; those
directories may remain.

See the [project README](https://github.com/WolalaQAQ/PicoET-Enhance#compatibility-and-verification)
for compatibility and validation details. The package contains the module scripts,
injector and hook payload; device libraries are supplied by the headset firmware.
