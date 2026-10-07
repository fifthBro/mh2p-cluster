# Audi MH2P field notes — October 2026

These are observations from one Audi MH2P/FPK virtual-cockpit installation. They are not an installer, firmware image, or claim that Porsche behavior carries over unchanged to Audi.

## Confirmed on the test vehicle

- Android Auto's secondary map stream can occupy the VC navigation map surface while Phone Apps and the center display remain functional.
- With the newer Audi-specific handover path, unplugging the phone restores the native VC map. Earlier patched-JAR experiments did not reliably do this.
- The VC can have more than one narrow map geometry. A narrow map at the left and one centered between gauges require different layer positions; map width alone is not enough to classify a layout.
- The same stream can survive VC page and layout changes when the Audi display handover is handled correctly. A transient return to the native map on page switch was an earlier defect, not an inherent limitation.
- A short press of the steering-wheel OK/menu key must remain available to the stock UI when the native map is selected. On the test vehicle, a separate long-press toggle between native and AA maps works; suppressing scroll **rotation** only while AA owns the VC prevents changes to the hidden native map's scale. Wheel press is unaffected.
- Intermittent initial H.264 smearing still needs diagnosis. Replugging the phone can clear it, but that is not a root-cause fix. The standalone metadata tracer in `tools/audi-vc-frame-trace/` was added to capture transport and decoder timing without retaining frames.

## Not solved or not included

- Audi HUD arrow/distance injection from Android Auto is not proven. Street/direction text can appear, but the arrow/distance region is controlled separately; BAP output alone has not established full HUD control.
- Inset values, display role, and handover timing are vehicle/firmware-specific. Do not treat one tested profile as a universal default.
- The original, reviewable key-toggle helper source is now included under `audi-mh2p/src/`. It still requires compatible lifecycle/key-dispatch call sites and the Audi DisplayManager composition methods in a current integration build; the proprietary/OEM classes and the exact vehicle JAR are not included.
- No fifthBro source files, proprietary OEM code/binaries, vehicle captures, or personal logs were copied into this contribution.

Contributors with an Audi MH2P/FPK unit are especially welcome to help with a clean layout-event API, a safe input bridge for the secondary AA display, and read-only evidence for HUD ownership. Please test only while parked; navigation display experiments must never be used as a safety-critical driving aid.
