# Cluster Integration MH2p / PCM5 and MHI2 / PCM4 — Android Auto & CarPlay navigation on the digital cluster

Renders **Android Auto** and **CarPlay** navigation directly on the instrument cluster of PCM5 / MH2P Porsche vehicles. Turn-by-turn arrows, live map view, seamless handoff back to native Porsche navigation when the phone disconnects.

This software is free software, developed and maintained in my spare time.

A lot of time goes into reverse engineering, development, testing and supporting different Porsche models and firmware versions.

If you find it useful, please consider supporting continued development.

Donations are entirely optional. The software is free and all features are available without payment.


[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/I2I01UMJL5)
> [**Download** latest release](https://github.com/fifthBro/mh2p-cluster/releases/latest)

> **Website:** https://fifthbro.github.io/

> **Support / Discussion:** ModKit Discord https://discord.gg/QAEkZmDh9

> **Status:** Stable release — CarPlay cluster rendering now live alongside Android Auto.

---

## What it does

When you plug in (or wirelessly connect) your phone, the digital instrument cluster becomes a second navigation display driven by the phone.

- **Android Auto cluster nav** — Google Maps, Waze, and other AA-compatible navigation apps render live in the cluster.
- **CarPlay cluster nav** — Apple Maps, Google Maps, Waze on iPhone stream to the cluster the same way.
- **Turn-by-turn on the physical dial** — maneuver icons, distance to next, lane guidance, roundabout exits, for both platforms.
- **Native maps come back automatically** — when the phone disconnects, the built-in Porsche navigation reclaims the cluster with no reboot required.
- **Independent of HMI state** — you can leave the main head-unit screen on native infotainment; the cluster keeps showing phone nav underneath.
- **Session-stable** — reconnects, park/drive cycles, HMI-off transitions all handled without the mirror dying.

---

## Supported vehicles

| Model | Head unit | Status |
|---|---|---|
| Cayenne (9Y0) | PCM5 / MH2P | ✅ Supported |
| Macan | PCM5 / MH2P | ✅ Supported |
| Panamera | PCM5 / MH2P | ✅ Supported |
| 992.1 (911) | PCM5 / MH2P | ✅ Supported |
| 718 (Cayman / Boxster) | MHI2 / PCM4 | 🚧 In progress — AA and CarPlay working, release next |

Model years: **2019–2021**. Firmware range: **9xxx–29xx**. Other firmwares can be enabled on request via the installer's OEM/firmware gate.

---

## Installation

Uses the **MH2P Mod Kit** (LawPaul): https://lawpaul.github.io/MH2p_SD_ModKit_Site/

> ⚠ **Warning:** This modifies Java files on your PCM5/MH2P unit. Only proceed if you understand the risks.

**Prerequisites**

- Car with PCM5/MH2P head unit
- SD card (FAT32)
- Android Auto or CarPlay compatible phone
- Android Auto [activated](https://lawpaul.github.io/MH2p_SD_ModKit_Site/) on the head unit
- It can be installed over the previous versions of the Cluster Integration
- If you need Android Auto and Carplay activated in the first place for details follow [MH2p_SD_ModKit_Site (https://lawpaul.github.io/MH2p_SD_ModKit_Site/)
- Cluster is a standalone install. If Android Auto and Carplay (full screen or not) were activated before you don't need to run them again

**Steps**

1. Download the latest release ZIP
2. Format the SD card as FAT32
3. Extract the ZIP to the root of the SD card

    The root of the SD card should look like this:

    <img src="https://fifthbro.github.io/dir.png" alt="SD card directory structure" width="450">

    **Using a Mac?** Additional steps are required:

    - [Video guide](https://youtu.be/7GyxGjJduF0)
    - [macOS installation guide](https://lawpaul.github.io/MH2p_SD_ModKit_Site/)

4. Start the vehicle
5. Insert the SD card
6. Within a few seconds, the head unit will reboot
7. The update installs automatically
8. When done, a prompt shows "Please remove update media"
9. Remove the SD card
10. Head unit reboots into normal mode with the mod installed

**Uninstall**

1. Rename `uninstall.txt.template` (in the mod folder on the SD card) to `uninstall.txt`
2. Re-run the ModKit update procedure with the SD card
3. `Update/uninstall.sh` runs automatically and restores originals from backup
4. After uninstall completes, remove `uninstall.txt` from the SD so it doesn't re-trigger

---

## Features

- **Full navigation support** — turn arrows, distance, road names, roundabouts, highway exits, lane guidance
- **Real-time updates** — proximity-zone throttling (veryFar → now) with heartbeat to hold the display against native nav
- **Intelligent unit detection** — automatic km/mi and LHD/RHD detection via platform services with JSON country fallback (60+ countries)
- **Multi-car config** — per-car overrides for Cayenne / 911 / Panamera / Macan / (Audi A7)
- **JSON configuration** — runtime-configurable without any reinstall; override via USB/SD card
- **Comprehensive logging** — dual-timestamp log with optional privacy hashing and external USB/SD output
- **Multi-platform** — Android Auto (DSI) + CarPlay (RGI / altScreen) in one integrated mod

---

## How it works (high level)

Android Auto navigation events arrive via the DSI service; CarPlay navigation arrives via the iAP2 RGI event stream. Both are translated to BAP (Bedien-und Anzeigeprotokoll) messages and pushed to the instrument cluster via `CombiBAPServiceNavi`. A heartbeat timer (default 2 s) continually re-sends the last state so the native cluster nav can't overwrite it.

CarPlay cluster rendering additionally uses a second video stream from the phone that is decoded on the head unit (H.264 via NvMedia) and composited onto the cluster's dedicated video surface. Android Auto uses an equivalent second-video-stream channel.

### Android Auto event → BAP maneuver translation

Maps AA `eventCode` (1–19) + `turnSide` + `angle` + `num` to a BAP maneuver descriptor. Direction is a 0–255 byte (0 = straight, 64 = left/90°, 128 = back/180°, 192 = right/270°).

| Event(s) | Translation |
|---|---|
| `event=3–5` (turns) | TURN, direction from `angle` or fallback to `turnSide` |
| `event=6` (U-turn) | UTURN |
| `event=7` (on-ramp) | TURN_ON_MAINROAD, direction from `turnSide` |
| `event=8/10` (off-ramp/merge) | EXIT_LEFT or EXIT_RIGHT based on `turnSide`; side=0 → FOLLOW_STREET |
| `event=9` (fork) | FORK_2 |
| `event=14` (STRAIGHT/KEEP) | `num==1` = Waze lane-keep → FOLLOW_STREET; else motorway exit → EXIT_LEFT/RIGHT |
| `event=11–13` (roundabout) | ROUNDABOUT_TRS_LEFT/RIGHT. RHD correction: `(540−angle)%360`; LHD: `(angle+180)%360` |
| `event=16/17` (ferry) | FERRY |
| `event=19` (destination preview) | Sets flag, falls through as FOLLOW_STREET |

### Distance update pipeline

1. **Throttle check** — minimum ms between sends per proximity zone (veryFar = 2000 ms down to now = 100 ms).
2. **Maneuver change detection** — key = `road|eventCode|turnSide`. New maneuver resets baseline distance and bypasses throttle for first update.
3. **Distance threshold** — minimum change required per zone (100 m at veryFar down to 5 m at now).
4. **Unit conversion** — metric: m below 100 m, tenths of km to 19.9 km. Imperial: yards below 161 m, tenths of miles to 9.9 mi.
5. **Bargraph** — `(1 − distance/maneuverInitialDistance) × 100`, clamped 0–100.
6. **Roundabout traversal** — force distance/bargraph to 0 during CONTINUE events to suppress erratic display inside roundabouts.

### Bargraph — Car variant auto-detection

When `bargraphMode="auto"`, the car type is queried at runtime:

| Car | carClass | generation | Resolved mode |
|---|---|---|---|
| Cayenne E3 | 5 | 3 | `"always"` |
| 911 992 | 6 | 8 | `"always"` |
| Panamera G2 | 6 | 2 | `"always"` |
| Macan | 4 | 1 | `"distance"` |
| Unknown | any | any | `"always"` (safe default) |

Explicit JSON values always override auto-detection.

---

## Configuration

The config file `cluster_config.json` is installed to `/mnt/app/eso/bin/apps/cluster/cluster_config.json`. To override without reinstalling, place a file on a USB stick or SD card — the app checks these paths first:

```
/fs/usb0_0/cluster_config.json
/fs/usb1_0/cluster_config.json
/fs/sda0/cluster_config.json
/fs/sdb0/cluster_config.json
```

### Logging

| Key | Type | Default | Description |
|---|---|---|---|
| `enableFileLogging` | bool | `true` | Master switch for all file logging. |
| `enableFileHashing` | bool | `false` | Write a second log with road names/music hashed for privacy. |
| `enableExternalLogging` | bool | `false` | Write logs to USB/SD if present; falls back to `logFilePath`. |
| `logFilePath` | string | `/tmp/cluster.log` | Plain log file path. |
| `hashedLogFilePath` | string | `/tmp/cluster_hashed.log` | Hashed log file path. |
| `logFileSize` | int (MB) | `50` | Max file size before rotation. Range: 1–100. |

### Features

| Key | Type | Default | Description |
|---|---|---|---|
| `enableRoundaboutLatching` | bool | `true` | Cache and hold roundabout exit direction across ENTER→CONTINUE→EXIT events to prevent arrow flickering. |
| `enableHeartbeat` | bool | `true` | Periodically resend last maneuver+distance to prevent native nav from overwriting. |
| `heartbeatInterval` | int (ms) | `2000` | Heartbeat period. Range: 100–10000. |
| `maneuverStateMask` | int | `0` | Bitmask for `updateManeuverState` calls. `0` = disabled. bit0=FOLLOW, bit1=PREPARE, bit2=DISTANCE, bit3=CALL_FOR_ACTION. Example: `15` (0b1111) = all states. |

### Units and display

| Key | Type | Default | Description |
|---|---|---|---|
| `forceImperial` | bool | `false` | Always use miles/yards, ignoring car settings. |
| `forceRHD` | bool | `false` | Always treat vehicle as right-hand drive. |
| `metricUnitThreshold` | int (m) | `100` | Below this, display switches from km to m. Range: 50–5000. |
| `imperialUnitThreshold` | int (m) | `161` | Below this (~0.1 mi), display switches from miles to yards. Range: 50–5000. |
| `destinationDisplayDuration` | int (ms) | `5000` | How long to hold the arrival maneuver on screen before clearing. Range: 0–10000. |

### Bargraph

| Key | Type | Default | Description |
|---|---|---|---|
| `bargraphMode` | string | `"auto"` | `"auto"` — detect car variant at runtime. `"always"` — send both text and bargraph. `"distance"` — text only. `"dynamic"` — bargraph only within `dynamicBargraphDistance`. |
| `dynamicBargraphDistance` | int (m) | `100` | In `"dynamic"` mode: distance below which bargraph replaces text. Range: 10–500. |
| `dynamicBargraphPercent` | int (%) | `50` | In `"dynamic"` mode: for short maneuvers (<2× threshold), switch point as % of initial distance. Range: 10–90. |

### Dynamic thresholds

Controls how aggressively distance updates are sent per proximity zone.

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

List of country overrides for when locale cannot be detected from platform services. `forceImperial` and `forceRHD` override this table entirely.

```json
{ "code": "GB", "name": "United Kingdom", "rhd": true,  "imperial": true  }
{ "code": "US", "name": "United States",  "rhd": false, "imperial": true  }
{ "code": "DE", "name": "Germany",        "rhd": false, "imperial": false }
```

| Field | Type | Description |
|---|---|---|
| `code` | string | ISO 3166-1 alpha-2 country code |
| `name` | string | Human-readable name (informational only) |
| `rhd` | bool | `true` if drive on the left (right-hand-drive) |
| `imperial` | bool | `true` if distances should be shown in miles/yards |

---

## Troubleshooting

**Bug reports**

1. Format a FAT32 SD card
2. Put SD in the head unit before turning ignition on
3. Drive, then when you turn ignition off pull the SD card
4. Look in the log for the event that was wrong
5. Match it to the same event in the hashed log (timestamps help)
6. Share the hashed log + expected vs. actual cluster behaviour on the Discord

**Known limitations**

- RHD/LHD and metric/imperial coverage still expanding — report issues
- Roundabout accuracy limited by whatever the phone sends (Waze in particular can lack exit angles)
- Data granularity and update rate limit accuracy on very short maneuvers
- Some glitches are inherent to Android Auto / CarPlay themselves

---

## Credits
- **LukaDev** — Managing screens on VW/MIB2 and CarPlay/GAL hacking: https://github.com/luka-dev/mib2q-carplay-rgi/
- **One1Blt** — Android Auto / VNC to MIB2 rendering: https://github.com/OneB1t/VcMOSTRenderMqb
- **adi961** — Turn-by-turn to VC integration: https://github.com/adi961/mib2-android-auto-vc
- **LawPaul** — MH2P Modkit: https://lawpaul.github.io/MH2p_SD_ModKit_Site/
- **CosminBC** — MH2P/MhHI2 Audi/VW commercial VC: https://altscreen.mib-helper.com/
- **litdreams10** — General platform knowledge, Android Auto testing
- **ModKit Discord community** — Testing, bug reports, cross-car validation: https://discord.gg/QAEkZmDh9

---

## Links

- Website: https://fifthbro.github.io/
- Latest release: https://github.com/fifthBro/mh2p-cluster/releases/latest
- MH2P Mod Kit (LawPaul): https://lawpaul.github.io/MH2p_SD_ModKit_Site/
- Discussion: ModKit Discord: https://discord.gg/QAEkZmDh9

---

*Not affiliated with Porsche, Volkswagen, Audi, or Google. This software is free and not for commercial use.*
