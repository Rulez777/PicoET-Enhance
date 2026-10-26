# Personal tongue-direction prototype (research only)

An opt-in feasibility implementation of tongue left/right tracking for one owner on a rooted PICO4 Enterprise A8110 / PICO OS5.13.7. NOT an official release, general model or production-ready tracker.

## Current implementation

- Reads the400x400 grayscale mouth Mat at the existing trackor hook. Reuses all upstream firmware guards and root-file protections.
- Compensates small image translations using the saturated nose-like region. The latest candidate uses a small CNN (3convolutions+2fully-connected layers,1x48x64input) instead of personal template matching. Native C++ inference has no PyTorch/ONNX dependency on the headset.
- Estimates continuous signed left/center/right position from relative similarities, with~80ms EMA and120ms hold for isolated uncertain frames. These are similarity-derived values, NOT measured anatomical coordinates.
- Native PICO TongueOut remains a lower bound. A negative-trained model fallback may maintain extension when native TongueOut loses an extreme pose; confidence enters at0.45; weaker positive evidence may retain it. Smile/closed-mouth samples are negative templates.
- Nominal processing cap20Hz. No measured20Hz guarantee, latency or frame-cost benchmark.
- Live processing stores/transmits parameters only. Optional bounded image capture is separately armed by a root-owned file; per-label filenames prevent silent collisions after a service restart.
- Streams24-byte TNG2 records over abstract Unix socket pico_tongue_live. Authenticated ADB forwards local TCP19031 to it. No inbound Windows firewall exception or raw-image stream is needed.
- Companion receiver checks record structure/finite values, bounds per-update work, reconnects after service restarts, and neutralizes model output after350ms without data. TNG2 packs explicit-negative/uncertain flags into the high two sequence bits (low30bits are the counter). An explicit negative clears immediately; uncertain frames may hold the last positive at most250ms.

## Evidence and remaining failures

The owner independently confirmed correct left/right motion in a worn VRChat session and confirmed continuous motion looked smooth. An early binary-presence override caused smile false positives and weak-extension retraction. Keeping native presence fixed the observed smile test (nativeOut0/modelOut~1/finalOut0) and the owner reported normal extension worked.

Extreme left/right still retracted in a subsequent live test. A held extreme-right pose showed nativeOut dropping from~0.36 to~0.018. Additional negative/extreme templates, confidence fallback and then image translation normalization were added. The owner retested the aligned version: extreme left/right still retract. They report cheek motion physically moves the headset; whether stabilizing the headset changes this is still under investigation. Do not describe the extreme-position issue as solved until that test passes.

Template same-burst withheld check:35/36 class matches; its wrong frame is outside the MSE rejection threshold. CNN same-burst validation:35/36; augmented variants351/360. The CNN misclassified one held-out extreme-right frame as hidden. This is NOT independent accuracy. No multi-user, re-donning, speech, lighting, long-session or full head-motion validation. Tongue up/down is not implemented. A ShadowHook island allocation failure occurred once; a clean restart with Bridge stopped activated the same code, but the exact cause is unproven.

## Calibration and reproduction

72 unique owner-labelled grayscale frames:32 in Capture03Final,16 in Capture04Extra,24 in Capture06Extremes. SmileNegative is a duplicate excerpt of the eight smile negatives, included for convenience. The owner explicitly consented to sharing these mouth/tongue images with the developer for this feature. No hardware serials, credentials or unrelated private files are included. No additional blanket dataset license is asserted.

Python requires numpy and Pillow. From this folder run offline_baseline.py, then export_live_model.py, export_live_model_v2.py, export_live_model_v3.py, export_live_model_v4.py. These scripts reproduce the template baseline. Then run train_tiny_cnn.py (requires PyTorch; CUDA optional) to regenerate hook/src/tongue_cnn_weights.h for the latest candidate. Training uses the first four frames of each held-pose burst and checks the remaining four. The latest model is personal and fit-specific.

Build with the repository's hook/build-hook.ps1 using Android NDK/CMake/Ninja. Do not bypass the repository's firmware hashes, recovery guards or root controls. Back up the original payload and hashes.sh before replacing anything. The source revision reviewed here was cb464d2a65b4d0dd0eba7c4a83305b3111f85c35.

For live opt-in create a root-owned0600 file /data/local/tmp/tongue-live.ip containing any valid private IPv4 (legacy enable-file format; no UDP is used). For PC integration add pc-integration/TongueReceiver.cs to UnifiedPicoModule and apply UnifiedPicoTrackingModule.patch, building with pinned VRCFT5.4.5 SDK. Create tongue-experiment.enabled beside the module containing an absolute writable path for its JSON diagnostic file. Forward `adb forward tcp:19031 localabstract:pico_tongue_live` over an already-authorized connection. Re-forward/re-authorize after reconnect/reboot if needed; never expose ADB to the Internet.

Receiver checks: `dotnet run --project pc-integration/tests/ReceiverChecks.csproj -c Release`. They cover TCP fragmentation, malformed/NaN rejection, high-confidence fallback, negative reset, bounded uncertain hold, fractional gating and stale neutralization.

Disable by removing the headset enable file and PC marker; restore the backed-up payload/hashes and companion DLL to return fully to stock. Production development should replace personal templates with robust tongue segmentation/regression and extend BridgeSplit with negotiated fields instead of relying on ADB.

Latest synchronized held-left check: PC Out1,Left~0.80-0.92,Right0 despite nativeOut~0.00-0.25. Owner confirmed the avatar tongue was visible in this check, while still reporting intermittent disappearance. Physically holding the headset did not remove the problem. TNG2 bounded uncertain holding subsequently added; final worn retest pending.

## CNN candidate validation boundary

The owner reported template tracking remained inconsistent (right-medium sometimes worked while left failed), so the current candidate was replaced with a small CNN trained on36frames with random translations, rotations, scales, contrast and noise;36same-burst frames were withheld. Class softmax temperature2 supplies continuous left/center/right weights. Hidden probability>0.85 is an explicit negative; visible mass>0.70 is a positive; other inputs are uncertain.

Nativearm64-versusPyTorch temperature2 outputs agreed within1.19e-7 absolute on four basic-class images. This validates numerical implementation, not real-world tracking accuracy. Native/PC builds and receiver checks pass. During the first CNN test VRCFT had closed; it was restarted and fresh PC/right1/out1 was then confirmed. Independent owner feedback on this candidate remains pending. These limits are intentional: this remains a research draft, not a completed general tongue tracker.
