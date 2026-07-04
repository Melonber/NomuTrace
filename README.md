# NomuTrace

**Lightweight security telemetry agent for MMU-less FreeRTOS embedded devices.**

> Status: **early prototype** — validated on ESP32-S3.

NomuTrace runs on constrained, MMU-less microcontrollers (ESP32-S3 today; STM32 / nRF52 targeted) and emits a small set of security-relevant runtime signals over a compact binary protocol. A host-side collector ingests those signals and forwards them to a SIEM for detection and alerting. The design goal is a footprint small enough to drop into an existing FreeRTOS application without disturbing it, while producing telemetry that maps cleanly onto industrial security standards (IEC 62443-4-2, EU CRA, NIS2).

---

## Architecture

```
┌────────────────────┐      6-byte binary      ┌──────────────┐      ┌──────────────┐
│  ESP32-S3           │   frames over MQTT/TCP  │  Collector   │      │   SIEM       │
│  NomuTrace agent    │ ──────────────────────► │  (Python,    │ ───► │  (Wazuh +    │
│  (ESP-IDF, C)       │                         │   aiomqtt)   │      │  custom      │
│                     │  store-and-forward      │              │      │  rules)      │
│  boot / heap /      │  4 KB ring buffer       │  decode +    │      │              │
│  stack / socket /   │                         │  normalize   │      │  alerting    │
│  wifi signals       │                         │              │      │              │
└────────────────────┘                         └──────────────┘      └──────────────┘
        edge device                               host / VPS              host / VPS
```

- **Firmware (`firmware/`)** — ESP-IDF application in C. Collects telemetry, encodes each event into a fixed 6-byte binary frame, buffers it in a store-and-forward ring buffer, and ships it over the transport layer.
- **Collector (`collector/`)** — Python edge collector (aiomqtt) that decodes the binary frames and normalizes them for downstream ingestion.
- **SIEM** — Wazuh with custom decoders/rules turns normalized events into detections. (Deployed on a VPS in the reference setup.)

---

## Telemetry signals

The prototype validates five signals, each isolated in its own module so it can be reviewed, tested, and compliance-mapped independently.

| Signal | Source module | What it captures |
|---|---|---|
| **Boot** | `boot_event.c/.h` | Device boot / reset events and cause |
| **Heap** | `heap_event.c/.h` | Free-heap / allocation health (memory exhaustion, leaks) |
| **Stack** | `stack_event.c/.h` | Per-task stack high-water mark (overflow risk) |
| **Socket** | `socket_event.c/.h` | Socket lifecycle / connection activity |
| **Wi-Fi** | `wifi_event.c/.h` | Wi-Fi association / disconnection events |

Shared plumbing:

- `event_queue.c/.h` — internal event queue between producers and the transport.
- `transport.c/.h` — outbound transport + store-and-forward (4 KB ring buffer).
- `sentinel_wire.h` — the 6-byte binary wire format shared by firmware and collector.
- `wifi_sta.c/.h` — station-mode Wi-Fi bring-up.
- `config.h` / `Kconfig.projbuild` — build-time and menuconfig configuration.

---

## Repository layout

```
NomuTrace/
├── collector/                  # Python edge collector (aiomqtt)
└── firmware/                   # ESP-IDF application (ESP32-S3)
    ├── CMakeLists.txt
    ├── partitions.csv
    ├── sdkconfig.defaults      # tracked project defaults
    ├── dependencies.lock       # pinned managed-component versions
    ├── idf_component.yml       # (in main/) component manifest
    └── main/
        ├── main.c              # entry point / agent wiring
        ├── boot_event.*        # signal modules
        ├── heap_event.*
        ├── stack_event.*
        ├── socket_event.*
        ├── wifi_event.*
        ├── wifi_sta.*          # Wi-Fi station bring-up
        ├── event_queue.*       # internal event queue
        ├── transport.*         # transport + store-and-forward
        ├── sentinel_wire.h     # 6-byte binary wire format
        ├── config.h            # build-time config
        └── Kconfig.projbuild   # menuconfig options
```

---

## Wire protocol

Events are serialized as a **compact fixed-size 6-byte binary frame** rather than JSON, to keep on-wire overhead and CPU cost negligible on constrained devices. The layout is defined once in `firmware/main/sentinel_wire.h` and mirrored by the collector's decoder, so both sides stay in sync. See that header for the authoritative field definitions.

---

## Building the firmware

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) (matching the version used to generate `dependencies.lock`).

```bash
cd firmware

# select the target once
idf.py set-target esp32s3

# configure (broker address, credentials, feature toggles)
idf.py menuconfig

# build, flash, and watch logs
idf.py build flash monitor
```

Managed components are **not** committed — on first build the ESP-IDF component manager restores them from `idf_component.yml` + `dependencies.lock`, so the build is reproducible without vendoring `managed_components/`.

## Running the collector

```bash
cd collector
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt   # aiomqtt, etc.
python collector.py
```

Point the collector at the same MQTT broker the firmware publishes to, then forward its output into your SIEM (Wazuh in the reference setup).

> Configuration specifics (broker host, topics, credentials) belong in your local config / environment and should not be committed.

---


## Roadmap

- Additional signal modules beyond the current five.
- Package the agent as a reusable ESP-IDF component (`components/nomutrace/`) for drop-in integration into third-party firmware.
- Port beyond ESP32-S3 to other MMU-less FreeRTOS targets (STM32, nRF52).
- Hardened transport (TLS / authenticated MQTT) and signed telemetry.

---

## License

See [`firmware/LICENSE`](firmware/LICENSE). *(Update this line with the actual license name once finalized.)*
