# Audi MH2P VC frame-metadata tracer

This is an independent, read-only QNX utility for diagnosing intermittent Android Auto map smearing on an Audi MH2P virtual cockpit. It samples the existing H.264 shared-memory ring and records byte-rate, Annex-B NAL counts (including IDR/SPS/PPS), observer overruns, resets, and decoder status messages. It does **not** save video frames or alter the GAL/video path.

The source is provided for review and adaptation. It was tested on one Audi MH2P installation in October 2026; it is not a smearing fix and is not a general vehicle compatibility claim.

Build with the matching QNX ARMv7 toolchain:

```sh
qcc -Vgcc_ntoarmv7le -O2 -Wall -Wextra -Werror -o vc_frame_trace vc_frame_trace.c
```

Run only in a controlled development setup with a writable log destination:

```sh
./vc_frame_trace --log /tmp/vc_frame_trace.log --seconds 60
```

The default log location is specific to the test vehicle. Override it with `--log` on other installations. Logs rotate at approximately 2 MiB per segment; inspect and redact them before sharing, since copied decoder status lines may contain platform details. An `OVERRUN` means this observer missed ring data, **not** necessarily that the decoder dropped frames.

No vehicle firmware, patched JAR, ModKit payload, credentials, logs, or third-party source is included here. The ring header layout is a compatibility interface to the upstream shared-memory producer, not a copy of its implementation.
