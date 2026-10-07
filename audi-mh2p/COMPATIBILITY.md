# Captured build identities and upstream work remaining

These SHA-256 values identify the **one working car's files**, not downloadable artifacts. They are here to prevent mixing an Audi handover JAR with an older hook, renderer, or different OEM firmware. The exact files remain in the owner's private backup; no OEM binaries, private key, or full firmware copy is in this pull request.

| Component | Captured SHA-256 | Notes |
| --- | --- | --- |
| Audi `5_8` config | `C30E7F883EAE49859A99638A6B34F27446AD8F649458DC18540855FDE9144170` | Published here at `config/cluster_config.audi-5_8.json` |
| Cluster renderer | `E1B3D4B680017B84F3A8E8901FD28A16F7E0D756051D739254E0093E28A9BCA5` | Current car; public `main` does not promise a byte-identical rebuild |
| GAL hook | `1E173663791BC58B4D73ED138FFA574990391CB74B6C4B15AED01A9F0CA0C7A5` | The experimental sidecar checks this exact build |
| DIO hook | `4DAD5A4A4E97D69F495CA819A79A82635BFA1ABAE98FCB0E6B5E39BE23884A8C` | CarPlay path, not the AA VC handover fix |
| Current integration JAR | `0FDB22DE74CBF1EDE8FB1D7250DC7AC695E548C2344735D9072DD7847ADDDC42` | Later HUD diagnostic variant; no confirmed HUD-arrow benefit |
| Vehicle-tested OK/menu V2 JAR | `B34C0287D161BB4459150C97277D497905FA97AF11C85679EC01C792B36620EB` | Tested wheel/menu behavior; derived from compatible Audi integration JAR |
| OEM GAL executable | `60C8CECE138887B1BFB22F657F314C50CB09FA7FB8F454A54AA45BAA9273A75F` | Must come from owner's matching firmware |
| OEM DIO executable | `7D13ECA9015D33F7C250BD15C7CBF70543A387359BB4F714BCC6078D68E04C1A` | Must come from owner's matching firmware |
| Patched NaviCompass JAR | `90F63C66430872281D7544AC7C1CE0DB803A544BD3208894C2124A7088FB8C84` | Audi/OEM-derived; not included |

## What fifthBro and contributors would need to ship a complete Audi release

1. Publish/build the current Audi-capable integration source and matching renderer/GAL hook from a tagged revision. The public `main` snapshot used for PR #17 predates the field-tested handover JAR, so merely using its release ZIP plus the JSON here is not equivalent to the working car.
2. Expose and test the Audi DisplayManager layer-62 composition/restore behavior (`activateClusterMirror62()` and `restoreClusterMapFromMirror62()` in the field build), including VC page and layout rearm. The layer-33 native map and layer-62 AA video have separate positioning. Do not assume Porsche composition methods are present on stock Audi firmware.
3. Integrate the original wheel helper at the AA lifecycle and Audi key-dispatch boundaries, or replace it with an upstream-owned equivalent. Ensure native-map short OK is replayed, AA-selected wheel **rotation** is consumed only on VC nav screen, wheel press/other pages are untouched, and long OK switches maps.
4. Package a ModKit/installer with car/firmware detection, OEM-file backup, reversible install, and parked regression checks. The public repo must obtain OEM files from the owner's car or firmware rather than bundling them.
5. Keep the smearing tracer optional and investigate keyframe/decoder recovery before choosing a frame-rate cap. Do not label the unresolved HUD arrow/distance path as functional on Audi.

The private local recovery bundle is deliberately more complete than this PR because it contains exact car snapshots. It must **not** be copied into the public repository wholesale.
