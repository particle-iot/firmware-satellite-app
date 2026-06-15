# M635E NTN Blueprint Demo

An always-on satellite (NTN) demonstration blueprint for the Particle M635e platform. This application demonstrates LTE-M ↔ NTN fallback behaviour for permanently powered devices, and doubles as a rich NTN diagnostic tool for evaluating satellite connectivity, antenna placement, and network transitions.

> **Note:** This is intentionally an *always-on* demo. Power management, deep sleep, battery optimisation, and low-power modem operation are excluded from this release to reduce complexity and maximise visibility into system behaviour.

## Features

- **LTE-M operation** with periodic publishing
- **NTN (satellite) operation** with location-assisted acquisition
- **LTE → NTN fallback** when LTE publishes haved failed beyond a configured timeout or LTE connectivity is not available
- **NTN → LTE recovery** via periodic LTE retry
- **eSIM profile switching** between LTE and NTN profiles
- **Single publish abstraction** that routes to LTE or NTN automatically
- **Verbose USBSerial diagnostics** — signal quality, satellite acquisition, state transitions, publish results, and timing estimates
- **Simulated LTE loss mode** for testing fallback without leaving coverage
- **Hardware watchdog** protection against stuck states
- **Prebuilt NTN-first binary** for immediate experimentation

## Requirements

### Hardware

- Particle **M635e** (M-SoM with NTN support)
- NTN-capable antenna (see Particle docs for more info)
- Optional GNSS antenna
- USB connection for USBSerial debug output
- Permanent power supply (this is an always-on application)

### Software

- **Device OS 6.4.2** or later
- eSIM profiles for both LTE and NTN installed on the device (profiles may or may not be activated — the application handles both cases). These are installed when going through setup.particle.io
- Does **not** depend on the Location API

## Quick Start

1. Connect the NTN antenna to the `CELL` connector on the M635e SOM.
2. Go to [blueprints.particle.io/](https://blueprints.particle.io/m635e-ntn/) and select "Use this blueprint" and then "Deploy to my device"
3. Go through the process at setup.particle.io to configure your device, installed the eSIM profiles and finally, this application
4. Open a serial terminal to view USB Serial debug output ("particle serial monitor" from the CLI). As soon as programming is completed, it will begin to search for NTN satellites and connect to the Particle cloud. The default configuration is **NTN-first / NTN-only publishing**, so satellite behaviour is demonstrated immediately.
5. Make sure you have a clear view of the sky (outdoors) for the device to connect. Initial Satellite registration can take up to 10 minutes. 

## Customizing the Demo

1. Download this blueprint from github and open in Particle Workbench
2. Select M-SoM as the device type and then compile and upload the program by plugging in the device to your computer over USB
3. Modify the `env.json` parameters to update if you are publishing from cellular, satellite or both!

On every boot the application resets all timers and the state machine, enables the watchdog, and reinitialises the modem.

## Antenna Guidance

NTN connectivity is highly sensitive to antenna selection and placement:

- Use an antenna rated for the NTN band with clear sky visibility.
- Place the antenna with an unobstructed view of the sky — away from metal enclosures, windows with coatings, and overhangs.
- Use the UART satellite acquisition and signal-quality output to compare placements empirically — this application is designed as a placement-evaluation tool.

## Configuration

All runtime behaviour is defined in a single top-level `env.json` file. Workbench builds each key/value pair into the application binary as an environment variable, and the firmware reads them at boot (see `src/app_config.cpp`). Any variable that is missing or invalid falls back to the compiled default in `src/app_config.cpp`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `FEATURE_LTE_ENABLED` | bool | `false` | Allow LTE-M as a connectivity stack. |
| `FEATURE_NTN_ENABLED` | bool | `true` | Allow Satellite NTN as a connectivity stack. At least one of the two `FEATURE_*_ENABLED` flags must be true; LTE will be enabled if both are false. |
| `START_ON_CELLULAR` | bool | `false` | Which radio the device boots on. `true` = LTE-M; `false` = NTN (useful for NTN-first demos). |
| `LTE_PUBLISH_INTERVAL_S` | uint | `60` | Seconds between publishes while on LTE-M. |
| `NTN_PUBLISH_INTERVAL_S` | uint | `300` | Seconds between publishes while on NTN. Do not set below `30`. |
| `VITALS_INTERVAL_S` | uint | `600` | Seconds between periodic device-vitals publishes. Vitals are always published once on (re)connect regardless of this value; `0` disables the periodic refresh (on-connect only). |
| `NTN_MAX_PAYLOAD_SIZE` | uint | `256` | Max on-wire frame size (header + body) for outbound NTN publishes. |
| `CELLULAR_DISCONNECTED_TIMEOUT_S` | uint | `600` | Seconds disconnected on LTE before switching to Satellite. There is no cellular "connected" timeout — if LTE is up, we stay. |
| `SATELLITE_CONNECTED_TIMEOUT_S` | uint | `600` | Seconds connected on Satellite before switching back to test Cellular again. |
| `SATELLITE_DISCONNECTED_TIMEOUT_S` | uint | `600` | Seconds disconnected on Satellite (including while still acquiring — SEARCH/LIMSRV before attach) before switching back to Cellular. NTN attach can take minutes, so don't set this too low or the device gives up before it ever connects. |
| `FORCE_CELLULAR_TO_SATELLITE_SWITCH` | bool | `false` | Bench testing only. When true, the LTE→NTN switch fires purely on `FORCE_C2S_SWITCH_TIMEOUT_S` after radio enable, ignoring LTE connection state. |
| `FORCE_SATELLITE_TO_CELLULAR_SWITCH` | bool | `false` | Bench testing only. When true, the NTN→LTE switch fires purely on `FORCE_S2C_SWITCH_TIMEOUT_S` after radio enable, ignoring NTN connection state. |
| `FORCE_C2S_SWITCH_TIMEOUT_S` | uint | `600` | Force-mode timeout for the LTE→NTN switch. Ignored unless `FORCE_CELLULAR_TO_SATELLITE_SWITCH` is true. |
| `FORCE_S2C_SWITCH_TIMEOUT_S` | uint | `600` | Force-mode timeout for the NTN→LTE switch. Ignored unless `FORCE_SATELLITE_TO_CELLULAR_SWITCH` is true. |
| `USE_ONBOARD_GNSS_FOR_LOCATION` | bool | `false` | Where the NTN location fix comes from. `false` = use the `PARTICLE_LOCATION_FIXED` coords below; never query the GNSS engine (no-antenna devices). `true` = use the onboard GNSS engine for up to `ONBOARD_GNSS_FIX_TIMEOUT_S`, then fall back to those coords. |
| `ONBOARD_GNSS_FIX_TIMEOUT_S` | uint | `60` | Maximum seconds to wait for a GNSS fix when `USE_ONBOARD_GNSS_FOR_LOCATION` is `true` before giving up and using the fixed coords. Unused when it is `false`. |
| `PARTICLE_LOCATION_FIXED` | string | `"44.92653,-93.39767,283.0"` | Fixed location as `"<latitude>,<longitude>,<altitude>"` in decimal degrees / meters. Used directly when GNSS is disabled, and as the fallback when GNSS is enabled. Warned about if missing/invalid while `USE_ONBOARD_GNSS_FOR_LOCATION` is `false`. |

### Example Configurations

**NTN-only (default shipped config)** — demonstrates satellite connectivity immediately:
- LTE disabled, NTN enabled, NTN-first publishing.

**LTE-only** — baseline cellular operation:
- LTE enabled, NTN disabled.

**Hybrid (LTE primary, NTN fallback)** — real-world deployment pattern:
- Both enabled. LTE is used until no successful publish occurs within the failure timeout (default 15 min), then the device switches eSIM profile and attaches to NTN. LTE is retried periodically; on restoration the device returns to LTE.

Use the **fake LTE loss** option to exercise the hybrid fallback lifecycle without leaving LTE coverage.

## How It Works

### Connectivity Lifecycle

```
                              ┌──────┐
                              │ Boot │
                              └──┬───┘
                                 │ radioEnable(start radio)
                                 ▼
                       ┌──────────────────┐
                       │ AcquireLocation  │      (one-shot, at boot only)
                       └──────────┬───────┘
        radioEnabled() == CELLULAR│ (else: SATELLITE)
                  ┌───────────────┴───────────┐
                  ▼                           ▼
        ┌──────────────────┐        ┌──────────────────┐
        │ CellularConnect  │        │ SatelliteConnect │
        └────────┬─────────┘        └────────┬─────────┘
                 │ Particle.connected()      │ satellite.connected()
                 ▼                           ▼
        ┌──────────────────┐        ┌──────────────────┐
        │  CellularOnline  │        │ SatelliteOnline  │
        └────────┬─────────┘        └────────┬─────────┘
                 │ cellularShouldSwitch      │ satelliteShouldSwitch
                 │ ToSatellite()             │ ToCellular()
                 ▼                           ▼
        ┌──────────────────┐        ┌──────────────────┐
        │ SwitchToSatellite│        │ SwitchToCellular │
        └──────────────────┘        └──────────────────┘
        radioEnable OK              radioEnable OK
        → SatelliteConnect          → CellularConnect

   (any radioEnable / satellite.begin() failure → Fault → Boot)
```

**LTE failure definition:** LTE is considered unavailable when no successful LTE publish occurs within the configured timeout window — even if registration, attach, or partial connectivity succeeds.

### Publish Abstraction

The application exposes a single publish call that routes to the LTE or NTN stack based on the current operating mode:

```cpp
publisher.publish("event_name", eventData);
```

This layer centralises rate limiting (1 message / 30 s on NTN), payload sizing (256-byte NTN limit), retries, logging, metrics, and publish accounting. Message types include vitals payloads, event publishes, and subscribes (all rate-limited on NTN).

### Location Handling

NTN attach requires location. `USE_ONBOARD_GNSS_FOR_LOCATION` selects how it is obtained:

1. `false` — use the `PARTICLE_LOCATION_FIXED` coordinates (`"lat,long,altitude"`) directly; the GNSS engine is never queried (no-antenna devices).
2. `true` — use the onboard GNSS engine for up to `ONBOARD_GNSS_FIX_TIMEOUT_S`, then fall back to the `PARTICLE_LOCATION_FIXED` coordinates if no fix is obtained.

Acquired location is cached and reused on each NTN attach attempt.

### Watchdog & Recovery

The application watchdog protects against stuck connection state machines, blocked attaches, blocked location acquisition, profile-switching hangs, and publish loop stalls. Repeated attach failures escalate to a modem reset.

### LED Behaviour

When the NTN radio is enabled, the system LED is overridden to clearly signal NTN acquisition status:
- Solid Green: NTN is searching 
- Solid Cyan: NTN is successfully registered
When on Cellular, the LED retains the normal particle behavior (ie blinking green for network registration, breathing cyan once registered and connected to the Particle cloud)

## Debug Output Interpretation

The USBSerial output is designed as a continuous NTN diagnostic environment. Expect to see:

- **Startup banner** — full runtime configuration at boot
- **Operating mode** — current mode (NTN vs LTE) reported continuously
- **Signal metrics** — LTE RSSI/RSRP/SINR and NTN signal quality where available
- **Satellite activity** — acquisition progress and visible satellites
- **State transitions** — connection state machine changes, eSIM profile switches, modem transitions
- **Publish results** — attempts, successes, failure causes, and success/failure counters
- **Timing estimates** — retry windows and time remaining before fallback/recovery transitions
- **Location status** — fixed vs acquired, acquisition progress

The goal is that you can understand what the modem is doing even when an NTN connection is unsuccessful.

## NTN Operational Expectations

- Satellite acquisition can take significantly longer than LTE attach; the timing-estimate log lines show where the device is in the process. First attach can take ~10 minutes.
- NTN payloads are limited to **256 bytes** and **1 message per 30 seconds** — the publish layer enforces both.
- NTN latency per packet is on the order of ~10 seconds per message. 
- A valid location is required before NTN attach.
- Clear sky view is essential; use the diagnostic output to validate placement.

## Skylo Conformance

This application is expected to serve as the demo app for Skylo conformance testing. The primary design goal is correct customer-facing behaviour; a conformance-specific branch may diverge as needed. The shipped configuration undergoes a Skylo compliance review.

## Troubleshooting

| Symptom | Things to check |
|---|---|
| No NTN attach | Sky visibility, antenna selection/placement, valid location available, NTN eSIM profile installed |
| Stuck during attach | Watchdog and modem-reset escalation will recover automatically; check state transition logs for the failure cause |
| Publishes failing on LTE | Expected to trigger NTN fallback after the timeout — check publish failure log lines for cause |
| Device never falls back to NTN | Verify NTN is enabled in config and the LTE failure timeout; use fake LTE loss mode to force the transition |
| No location | Set a fixed location in config, or ensure GPS has sky view |

## License / Support

Refer to Particle documentation and support channels for the M635e and NTN platform.
