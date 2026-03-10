# Android Auto (and soon CarPlay) Cluster Integration

Bridge Android Auto navigation from Porsche PCM5 / ~~VW / Audi~~ MH2P to instrument cluster displays.

> **Sponsor** https://ko-fi.com/fifthbro

> **Status:** Beta (v01027) — Testing compatibility across Porsche Cayenne, Macan, Panamera and 911

> **Download** [latest release](https://github.com/fifthBro/mh2p-cluster/raw/refs/heads/main/builds/AndroidAutoCluster_v01027_a38ec9a.zip)

---

## Overview

Translates Android Auto DSI navigation events into BAP (Board Access Protocol) messages for real-time turn-by-turn navigation on your instrument cluster. Native navigation monopolizes the cluster — this changes that.

- Intercepts Android Auto navigation events via DSI (Device System Interface)
- Converts them to BAP protocol messages
- Sends to cluster via `CombiBAPServiceNavi`
- Overrides native navigation with configurable heartbeat (default 2s)
- Supports both Google Maps and Waze

---

## Features

- **Full navigation support** — turn arrows, distance, road names, roundabouts, highway exits
- **Real-time updates** — proximity-zone throttling (veryFar→now) with heartbeat to override native nav
- **Intelligent unit detection** — automatic km/mi and LHD/RHD detection via platform services with JSON country fallback (60+ countries)
- **JSON configuration** — runtime-configurable without recompilation; override via USB/SD card
- **Comprehensive logging** — dual-timestamp log with optional privacy hashing and external USB/SD output
- **Java 1.4 compatible** — runs on the embedded QNX/OSGI platform without modern Java features

---

## Architecture

`AndroidAutoClusterIntegration` is a single Java class (Java 1.4 compatible) that bridges the Android Auto DSI event stream to the Porsche/Audi instrument cluster BAP navigation protocol.

### Data Flow

```
Android Auto (Phone)
        |
    DSI Events (Navigation, Media)
        |
AndroidAutoClusterIntegration
    +-- navFocusRequestNotification   -> start/stop heartbeat, set RGType
    +-- updateNavigationNextTurnEvent -> convertToBAPManeuver -> updateManeuverDescriptor
    +-- updateNavigationNextTurnDistance -> rate-limit -> unit convert -> updateDistanceToNextManeuver
    +-- Heartbeat Timer (every 2s)   -> resend cached maneuver+distance
        |
CombiBAPServiceNavi (OSGi)
        |
Instrument Cluster
```

### Service Injection

All platform services are injected via setters after construction:

| Service | Purpose |
|---|---|
| `CombiBAPServiceNavi` | BAP cluster output — maneuver, distance, RGStatus, destination |
| `ISysServices` | Unit system detection, car clock, car type/variant detection |
| `ICarCoreServices` | Drive-side detection via `exteriorLight().leftHandTraffic()` |
| `StorageMountHandler` | Remounts USB/SD R/W for external log writing |
| `ICarStatisticsService` | Injected but not currently used in active code |

### AA Event → BAP Maneuver Translation

Maps Android Auto `eventCode` (1–19) + `turnSide` + `angle` + `num` to a BAP maneuver descriptor. Direction is a 0–255 byte (0=straight, 64=left/90°, 128=back/180°, 192=right/270°).

| Event(s) | Translation |
|---|---|
| `event=3–5` (turns) | TURN, direction from `angle` or fallback to `turnSide` |
| `event=6` (U-turn) | UTURN |
| `event=7` (on-ramp) | TURN_ON_MAINROAD, direction from `turnSide` |
| `event=8/10` (off-ramp/merge) | EXIT_LEFT or EXIT_RIGHT based on `turnSide`; side=0 → FOLLOW_STREET |
| `event=9` (fork) | FORK_2 |
| `event=14` (STRAIGHT/KEEP) | `num==1` = Waze lane-keep → FOLLOW_STREET; `num==0` or `num>1` = motorway exit → EXIT_LEFT/RIGHT |
| `event=11–13` (roundabout) | ROUNDABOUT_TRS_LEFT/RIGHT. RHD correction: `(540−angle)%360`; LHD: `(angle+180)%360`. T-junction: 180°→0° when exit ≤ 2 |
| `event=16/17` (ferry) | FERRY |
| `event=19` (destination preview) | Sets flag, falls through as FOLLOW_STREET. DESTINATION symbol shown only on `event=0 valid=2` |

### Distance Update Pipeline

1. **Throttle check** — minimum ms between sends per proximity zone (veryFar=2000ms down to now=100ms). In "always" bargraph mode, bargraph-only updates bypass the throttle.
2. **Maneuver change detection** — key = `road|eventCode|turnSide`. New maneuver resets baseline distance and bypasses throttle for first update.
3. **Distance threshold** — minimum change required per zone (100m at veryFar down to 5m at now) before text updates. Bargraph always updates for smooth fill.
4. **Unit conversion** — metric: m below 100m, tenths of km to 19.9km. Imperial: yards below 161m, tenths of miles to 9.9mi.
5. **Bargraph** — `(1 − distance/maneuverInitialDistance) × 100`, clamped 0–100.
6. **Roundabout traversal** — force distance/bargraph to 0 during CONTINUE events to suppress erratic display inside roundabout.

### Heartbeat

A daemon timer fires every `heartbeatInterval` ms (default 2000ms) while navigation is active. It resends the last cached maneuver, distance, bargraph, and destination data to continuously override the cluster's native navigation display.

### Unit & Drive-Side Detection (3-tier each)

| Priority | Units | Drive side |
|---|---|---|
| 1 (highest) | `forceImperial` config flag | `forceRHD` config flag |
| 2 | `ISysServices.units().distance().setting()` (1=km, 2=mi) | `ICarCoreServices.exteriorLight().leftHandTraffic()` |
| 3 | JSON `countries` table via locale/system properties | JSON `countries` table `rhd` field |
| 4 (default) | Metric | LHD |

### Bargraph Auto Mode — Car Variant Detection

When `bargraphMode="auto"`, the car type is queried at runtime via `sysServices.config().carType()`:

| Car | carClass | generation | Resolved mode |
|---|---|---|---|
| Cayenne E3 | 5 | 3 | `"always"` |
| 911 992 | 6 | 8 | `"always"` |
| Panamera G2 | 6 | 2 | `"always"` |
| Macan | 4 | 1 | `"distance"` |
| Unknown | any | any | `"always"` (safe default) |

Explicit JSON values always override auto-detection.

### Roundabout Latching

Android Auto sends EXIT (event=13) first, then CONTINUE (event=12) × N through the roundabout. Latching caches the exit direction on event=11/13 and reuses it for all subsequent event=12 updates, preventing the arrow from flickering. Cleared on any non-roundabout event. Configurable via `enableRoundaboutLatching`.

### Logging

Two parallel log files: **plain** (full road names) and **hashed** (road names, music metadata replaced with `[HASH_nnn]` for privacy). Each entry has a dual timestamp: QNX boot time + real car clock from `ISysServices.clock().localTime()`. When `enableExternalLogging=true`, logs are written to USB/SD if writable, with automatic fallback to `/tmp`.

---

## Installation

**Based on MH2p Mod Kit** https://lawpaul.github.io/MH2p_SD_ModKit_Site/

> **Warning:** This modifies Java files on your PCM5/MH2P unit. Only proceed if you understand the risks.

**Prerequisites:**
- Car with PCM5/MH2P
- SD card
- Android Auto phone
- Android Auto <a href="https://lawpaul.github.io/MH2p_SD_ModKit_Site/">activated </a>

**Installation:**
- Download latest release
- Format SD card as FAT32
- extract zip file to the root of SD card
- start vehicle
- insert the SD card
- within few seconds, MH2p will reboot
- update installs automatically
- when update is done installing, a prompt will say "Please remove update media"
- remove update media from vehicle
- MH2p will reboot into normal mode with mods installed/uninstalled

**Build:**
Edit build script with paths in your environment and run: 
recompile.sh
or
recompile.bat

**Deploy and test:**
The easiest way is to just scp it to the target if you <a href="https://github.com/fifthBro/mh2p-ssh-access">enbaled ssh access</a>

---

## Configuration

The config file `androidauto_cluster_config.json` is embedded in the JAR. To override without reflashing, place the file on a USB stick or SD card — the app checks these paths first:

```
/fs/usb0_0/androidauto_cluster_config.json
/fs/usb1_0/androidauto_cluster_config.json
/fs/sda0/androidauto_cluster_config.json
/fs/sdb0/androidauto_cluster_config.json
```

### Logging

| Key | Type | Default | Description |
|---|---|---|---|
| `enableFileLogging` | bool | `true` | Master switch for all file logging. |
| `enableFileHashing` | bool | `false` | Write a second log with road names/music hashed for privacy. Requires `enableFileLogging`. |
| `enableExternalLogging` | bool | `false` | Write logs to USB/SD if present; falls back to `logFilePath`. |
| `logFilePath` | string | `/tmp/androidauto_cluster.log` | Plain log file path. |
| `hashedLogFilePath` | string | `/tmp/androidauto_cluster_hashed.log` | Hashed log file path. |
| `logFileSize` | int (MB) | `50` | Max file size before deletion and restart. Range: 1–100. |

### Features

| Key | Type | Default | Description |
|---|---|---|---|
| `enableRoundaboutLatching` | bool | `true` | Cache and hold roundabout exit direction across ENTER→CONTINUE→EXIT events to prevent arrow flickering. |
| `enableHeartbeat` | bool | `true` | Periodically resend last maneuver+distance to prevent native nav from overwriting AA data. |
| `heartbeatInterval` | int (ms) | `2000` | Heartbeat period. Range: 100–10000. |
| `maneuverStateMask` | int | `0` | Bitmask for `updateManeuverState` calls. `0` = disabled. bit0=state1 FOLLOW (>500m), bit1=state2 PREPARE (200–500m), bit2=state3 DISTANCE (50–200m), bit3=state4 CALL_FOR_ACTION (<50m). Falls back to nearest lower enabled state. Example: `15` (0b1111) = all states. |

### Units and Display

| Key | Type | Default | Description |
|---|---|---|---|
| `forceImperial` | bool | `false` | Always use miles/yards, ignoring car settings. For testing on metric testbenches. |
| `forceRHD` | bool | `false` | Always treat vehicle as right-hand drive, overriding all detection. |
| `metricUnitThreshold` | int (m) | `100` | Below this, display switches from km to m. Range: 50–5000. |
| `imperialUnitThreshold` | int (m) | `161` | Below this (~0.1 mi), display switches from miles to yards. Range: 50–5000. |
| `destinationDisplayDuration` | int (ms) | `5000` | How long to hold the arrival maneuver on screen before clearing. Range: 0–10000. |

### Bargraph

| Key | Type | Default | Description |
|---|---|---|---|
| `bargraphMode` | string | `"auto"` | `"auto"` — detect car variant at runtime (Cayenne E3/911 992/Panamera G2 → `"always"`, Macan → `"distance"`). `"always"` — send both text and bargraph. `"distance"` — text only. `"dynamic"` — text when far, bargraph only within `dynamicBargraphDistance`. Explicit JSON values override auto-detection. |
| `dynamicBargraphDistance` | int (m) | `100` | In `"dynamic"` mode: distance below which bargraph replaces text. Range: 10–500. |
| `dynamicBargraphPercent` | int (%) | `50` | In `"dynamic"` mode: for short maneuvers (<2x threshold), switch point as % of initial distance. Range: 10–90. |

### Dynamic Thresholds

Controls how aggressively distance updates are sent per proximity zone. Each zone has a **boundary** (upper edge in metres), **distance** threshold (minimum change to trigger a send), and **rateLimit** (minimum ms between sends). `now` has no boundary — catch-all below `veryClose.boundary`.

| Zone | Default boundary | Default distance | Default rateLimit |
|---|---|---|---|
| `veryFar` | 5000 m | 100 m | 2000 ms |
| `far` | 1000 m | 50 m | 500 ms |
| `approaching` | 500 m | 25 m | 250 ms |
| `near` | 200 m | 15 m | 200 ms |
| `close` | 100 m | 15 m | 150 ms |
| `veryClose` | 50 m | 10 m | 120 ms |
| `now` | — | 5 m | 100 ms |

```json
"dynamicThresholds": {
  "veryFar":    { "boundary": 5000, "distance": 100, "rateLimit": 2000 },
  "far":        { "boundary": 1000, "distance": 50,  "rateLimit": 500  },
  "approaching":{ "boundary": 500,  "distance": 25,  "rateLimit": 250  },
  "near":       { "boundary": 200,  "distance": 15,  "rateLimit": 200  },
  "close":      { "boundary": 100,  "distance": 15,  "rateLimit": 150  },
  "veryClose":  { "boundary": 50,   "distance": 10,  "rateLimit": 120  },
  "now":        {                    "distance": 5,   "rateLimit": 100  }
}
```

### Countries

List of country overrides for when locale cannot be detected from platform services. Countries not listed default to LHD metric. `forceImperial` and `forceRHD` override this table entirely.

```json
{ "code": "GB", "name": "United Kingdom", "rhd": true,  "imperial": true  }
{ "code": "US", "name": "United States",  "rhd": false, "imperial": true  }
{ "code": "DE", "name": "Germany",        "rhd": false, "imperial": false }
```

| Field | Type | Description |
|---|---|---|
| `code` | string | ISO 3166-1 alpha-2 country code |
| `name` | string | Human-readable name (informational only) |
| `rhd` | bool | `true` if vehicles drive on the left (right-hand drive countries) |
| `imperial` | bool | `true` if distances should be shown in miles/yards |
---

## Contributing
			
This project is the result of extensive reverse-engineering and testing on mh2p. Contributions welcome!

**Troubleshooting / bug reports**
- Format a FAT32 SD card
- Put SD in PCM before turning the ignition on
- Drive → afterwards when you turn the ignition off → pull SD card → look in the log for event that was wrong → match that event to the same one in the hashed log (time stamps can be useful)
- Share the hashed logs + expected vs actual cluster behaviour

**Known limitations**
- RHD/LHD and metric/imperial still requires testing
- Roundabouts limited (Waze lacks exit angles so they can be wrong)
- Data granularity and slow update rate limit accuracy
- Some glitches are inherent to Android Auto.

---

## Changelog

### v1027 — Beta candidate
Testing compatibility across Porsche Cayenne, Macan, Panamera and 911.

---

## Credits

- **One1Blt** — Android Auto/VNC to MIB2 rendering: https://github.com/OneB1t/VcMOSTRenderMqb
- **adi961** — Turn-by-turn to VC integration: https://github.com/adi961/mib2-android-auto-vc
- **LawPaul** — MH2P Modkit: https://lawpaul.github.io/MH2p_SD_ModKit_Site/
- **LukaDev** — Managing screens in VW/MIB2 and CarPlay/GAL hacking: https://github.com/luka-dev/mib2q-carplay-rgi/
- **litdreams10** — General platform knowledge, Android Auto testing

---

*Not affiliated with Porsche, Volkswagen, Audi, or Google. This software is free and not for commercial use.*
