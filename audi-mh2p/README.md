# Audi MH2P/FPK installation profile and additions

This directory collects the **Audi-specific, publishable parts** of the C8 A7 field implementation. It is intentionally separate from Porsche defaults. The tested vehicle runs MH2P US AUG35 P2873, Audi car class `5_8`, native Android Auto, and an FPK virtual cockpit. The same results are **not** established for another firmware, VC, or car.

## On-car result

These photos were supplied by the vehicle owner and show the working AA map on the Audi VC in two cluster layouts. They show the VC display result—not proof that AA arrows or distance work on the windscreen HUD. The native scale indicator at the lower right remains visible and is an unresolved item.

Full-width navigation layout, with the AA map and guidance card:

![Android Auto map on the full-width Audi virtual cockpit](images/audi-vc-aa-full-width.jpg)

Narrow-left navigation layout, with vehicle information occupying the right side:

![Android Auto map in the narrow-left Audi virtual cockpit layout](images/audi-vc-aa-narrow-left.jpg)

## What is actually working on the test car

| Behavior | Field result | Public ingredient |
| --- | --- | --- |
| AA secondary map on VC, native map returns after unplug | Confirmed | `cluster_config.audi-5_8.json`, current GAL/cluster integration, Audi display-manager composition |
| AA stream survives VC page/layout changes | Confirmed | Audi handover/rearm logic in the later integration build |
| Center Phone Apps and AA start after a forced GAL stop | Improved; long-term reliability still under observation | `gal-wrapper.sh` uses `exec gal.real` |
| Long OK toggles AA/native VC maps; short OK opens native menu; wheel rotation is ignored when AA owns VC | Confirmed with V2 JAR | Original `AudiClusterKeyDiagController.java` source in `src/` |
| VC frame metadata trace | Confirmed, read-only | `tools/audi-vc-frame-trace/` and `vc-frame-trace-persist.sh` |

The active `5_8` profile from the car is `codecRes=1080`, `dpi=180`, `insetTop=315`, `insetLeft=480`, `insetBottom=360`, `insetRight=480`, `rgType=4`; the GAL wrapper sets `GAL_CLUSTER_DISPLAY_TYPE=2` (AUXILIARY). Those are *observed settings*, not universal recommendations. The tested key helper uses VC nav screen `4000010`, key code `30` for OK/menu, wheel key code `20` with event ID `10403`, and an approximately 1.1-second hold. A different car must trace its own codes before using them.

## Files here

- `config/cluster_config.audi-5_8.json`: exact non-secret configuration captured on 2026-10-07. It includes the project's normal configuration data, so keep the repository's CC BY-NC-SA license and attribution.
- `scripts/gal-wrapper.sh`: the wrapper that keeps smartphone_integrator supervising the **real** GAL process, while loading the cluster hook and passive rotary sidecar. The sidecar does **not** give Google Maps zoom input.
- `scripts/cluster-persist.sh`: starts the cluster daemon without the old splash on every boot.
- `scripts/vc-frame-trace-persist.sh`: optional metadata-only tracer autostart.
- `src/de/audi/kbd/hmi/AudiClusterKeyDiagController.java`: our Java 1.4-compatible steering-wheel helper. It must be compiled against the vehicle/OEM API available to the builder. It is **not sufficient by itself**: the integration JAR must call `handle(...)` in the key dispatch path and `onMirrorCommand(...)` in the AA map lifecycle, and the resolved Audi DisplayManager must supply `activateClusterMirror62()` and `restoreClusterMapFromMirror62()`.
- `experimental/gal_cluster_rotary_nohook.c`: original source for the passive sidecar loaded by the captured wrapper. It is guarded to one stable GAL-hook build and waits for a local control command. Rotary/pinch tests did **not** zoom the Google Maps content on the VC, so this is not advertised as a feature. Do not reuse the hard-coded offsets with a different hook binary.

See [compatibility fingerprints](COMPATIBILITY.md) for the exact captured build identities and the release work needed for a genuinely repeatable install.

## Reproducing on a compatible, already-provisioned vehicle

1. Confirm exact MH2P firmware/car class, make an offline backup of every file that would be replaced, and verify recovery/SSH while parked. Do **not** install an AA/CarPlay enable mod on a car that already has factory AA/CarPlay.
2. Obtain a current fifthBro build/source that supports Audi displayable-62 composition and lifecycle handback. The repository's current `main` release/source is older than the build on the test vehicle; the config and helper here cannot add those missing methods to an old JAR. Obtain QNX build artifacts from a trusted source or compile the project's own `src/` with the proper QNX 6.5 ARMv7 toolchain. Obtain OEM `gal`, `dio_manager`, and `NaviCompass.jar` from **your own matching firmware**, not this repository.
3. Merge only the Audi `5_8` profile into the integration's installed `cluster_config.json`. Set GAL's secondary display role to AUXILIARY for this profile. Load the matching `gal_cluster.so` hook through `gal-wrapper.sh`, keeping the OEM GAL executable as `gal.real`; use the project’s compatible `cluster` renderer and cluster persistence service. Do not infer that a hook built from older `main` source is byte-compatible with the later Audi JAR.
4. Integrate/compile the original key helper into that JAR with the documented lifecycle and key-dispatch call sites. Its `switchMap` calls are reflection-gated and only work where the Audi DisplayManager has the two no-argument methods. If absent, the key toggle is unavailable; do not silently use the Porsche behavior.
5. Restart the MMI only while parked. Check Phone Apps, center-screen AA, VC video, layout/page changes, short and long OK, native map handback on unplug, and reconnect. If anything fails, restore the backed-up files before driving. The optional tracer is observational, not a fix.

This is a **source/profile/reproduction guide, not a one-click installer**. We cannot safely publish the exact live JAR, proprietary OEM executables/JARs, SSH credentials, or vehicle firmware captures. Therefore another owner cannot reproduce the entire field setup from this PR alone until the corresponding current fifthBro Audi build and its install path are public. Keeping that limitation explicit is more useful than distributing an installer that would run incompatible binaries.

## Remaining problems

- Intermittent VC smearing is unresolved. An October 7 trace saw no ring overrun or decoder error and showed sparse H.264 IDR keyframes; one visual self-clear was near a new IDR. That is correlation, not a demonstrated fix or reason to impose a frame cap.
- AA street/direction text can reach the HUD. AA arrows, distance, and junction graphics **without native guidance** remain unsolved; BAP FctID 23 and `rgType=4` alone have not made them appear reliably.
- The native scale bar can remain above AA video. Its generation/ownership is not solved on this MH2P/FPK combination.
- Dynamic wide/narrow AA content-inset renegotiation and actual Google Maps zoom input from the wheel are not implemented. The passive rotary sidecar is not a working zoom solution.

Please use parked, reversible tests and do not rely on this experimental display output for driving decisions.
