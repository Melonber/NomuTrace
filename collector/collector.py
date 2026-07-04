"""
Sentinel collector — prototype.

Receives binary frames from devices via MQTT, parses the header,
dispatches payload to the appropriate parser by event_type,
prints normalized ECS-like events to stdout.

Wire contract (see main/sentinel_wire.h on device):

    [ 6-byte header ][ binary payload ]

    header:
      offset 0..1  magic       = b"SN"
      offset 2     schema_ver  = uint8
      offset 3     event_type  = uint8  (1=boot, 2=socket, ...)
      offset 4..5  payload_len = uint16 LE

device_id is taken from MQTT topic: sentinel/events/{device_id}
"""
import os
from pathlib import Path
import asyncio
import json
import struct
import sys
from datetime import datetime, timezone
from typing import Callable

import aiomqtt

# ---------- Configuration ----------
MQTT_HOST = "0.0.0.0"
MQTT_PORT = 1883
TOPIC_FILTER = "sentinel/events/+"

EVENTS_FILE = Path.home() / "nomutrace" / "events.ndjson"

# ---------- Wire header ----------
HEADER_MAGIC = b"SN"
HEADER_SIZE = 6
# magic[2] | schema_ver(B) | event_type(B) | payload_len(H, LE)
HEADER_STRUCT = "<2sBBH"

# event_type registry — must match sentinel_wire.h on device
EVT_BOOT = 1
EVT_SOCKET = 2

# ---------- Wire payload formats ----------

# boot_event: little-endian, 40 bytes — mirror of boot_event_serialize().
BOOT_STRUCT = "<IIBBBB28s"
BOOT_SIZE = struct.calcsize(BOOT_STRUCT)  # 40

# socket_event: little-endian, 20 bytes — mirror of build_binary().
#   0..7   ts_us       int64
#   8..11  ip          uint32 (network byte order — NOT byte-swapped here)
#   12..13 port        uint16 LE
#   14     transport   uint8
#   15     family      uint8
#   16     allowlisted uint8
#   17..19 reserved    3 bytes
SOCKET_STRUCT = "<qIHBBB3s"
SOCKET_SIZE = struct.calcsize(SOCKET_STRUCT)  # 20


EVT_STACK_WATERMARK = 3

# stack_watermark: little-endian, 28 bytes — mirror of build_payload().
#   0..7   ts_us           int64
#   8..11  watermark_words uint32
#   12..15 stack_size_est  uint32
#   16..27 task_name       12 bytes
STACK_STRUCT = "<qII12s"
STACK_SIZE = struct.calcsize(STACK_STRUCT)  # 28

EVT_WIFI = 5

# wifi_event: little-endian, 24 bytes — mirror of build_payload().
#   0..7   ts_us           int64
#   8      sub_event       uint8   (1=connected, 2=disconnected)
#   9      channel         uint8
#   10..11 reason          uint16
#   12..17 bssid           6 bytes
#   18     rssi            int8
#   19..23 reserved        5 bytes
WIFI_STRUCT = "<qBBH6sb5s"
WIFI_SIZE = struct.calcsize(WIFI_STRUCT)  # 24

WIFI_SUB = {1: "connected", 2: "disconnected"}

# Subset of reason codes that are interesting for compliance.
# Full list is in esp_wifi_types.h, we map only the most important ones.
WIFI_REASONS = {
    0:   "unspecified",
    2:   "auth_expire",
    3:   "deauth_leaving",          # potential deauth attack
    4:   "disassoc_due_to_inactivity",
    8:   "left_network",
    15:  "4way_handshake_timeout",  # usually wrong password
    200: "beacon_timeout",          # AP stopped responding
    201: "no_ap_found",
    202: "auth_fail",
    203: "assoc_fail",
    204: "handshake_timeout",
    205: "connection_fail",
}

# ---------- Lookup tables ----------

RESET_REASONS = {
    0: "unknown", 1: "poweron", 2: "external", 3: "sw", 4: "panic",
    5: "int_wdt", 6: "task_wdt", 7: "wdt", 8: "deepsleep", 9: "brownout",
    10: "sdio", 11: "usb", 12: "jtag", 13: "efuse",
    14: "pwr_glitch", 15: "cpu_lockup",
}
FLASH_ENC = {0: "disabled", 1: "development", 2: "release", 3: "unknown"}
TRANSPORT = {0: "tcp", 1: "udp", 2: "other"}


EVT_HEAP = 4

# heap_event: little-endian, 16 bytes — mirror of heap_cb().
#   0..7   ts_us           int64
#   8..11  free_now        uint32
#   12..15 free_min_ever   uint32
HEAP_STRUCT = "<qII"
HEAP_SIZE = struct.calcsize(HEAP_STRUCT)  # 16

# ---------- Payload parsers ----------

def parse_boot(device_id: str, schema_ver: int, payload: bytes,
               received_at: str) -> dict | None:
    if len(payload) != BOOT_SIZE:
        print(f"[boot] {device_id}: expected {BOOT_SIZE} bytes, got {len(payload)}",
              file=sys.stderr)
        return None

    ts, uptime, reset, secure, fenc, _reserved, fw = struct.unpack(BOOT_STRUCT, payload)
    fw = fw.split(b"\x00", 1)[0].decode("utf-8", "replace")

    return {
        "@timestamp": received_at,
        "event": {
            "kind": "event",
            "category": ["host"],
            "action": "device_boot",
            "module": "sentinel",
            "dataset": "sentinel.boot",
        },
        "device": {"id": device_id},
        "host": {"uptime": uptime},
        "sentinel": {
            "schema_version": schema_ver,
            "reset_reason": RESET_REASONS.get(reset, f"unknown({reset})"),
            "reset_reason_code": reset,
            "secure_boot": bool(secure),
            "flash_encryption": FLASH_ENC.get(fenc, f"unknown({fenc})"),
            "firmware_version": fw,
            # ts_device — untrusted wall-clock time from device (pre-SNTP).
            # Kept for audit; authoritative time is @timestamp from collector.
            "device_timestamp": ts,
        },
    }


def parse_socket(device_id: str, schema_ver: int, payload: bytes,
                 received_at: str) -> dict | None:
    if len(payload) != SOCKET_SIZE:
        print(f"[socket] {device_id}: expected {SOCKET_SIZE} bytes, got {len(payload)}",
              file=sys.stderr)
        return None

    ts_us, ip_net, port, transport, family, allowlisted, _reserved = \
        struct.unpack(SOCKET_STRUCT, payload)

    # ip received in network byte order (as in lwIP). Convert to dotted quad.
    ip_str = ".".join(str((ip_net >> (8 * i)) & 0xff) for i in range(4))

    is_allowed = bool(allowlisted)

    event = {
        "@timestamp": received_at,
        "event": {
            "kind": "event",
            "category": ["network"],
            "type": ["connection", "start"],
            "action": "socket_connect",
            "module": "sentinel",
            "dataset": "sentinel.socket",
        },
        "device": {"id": device_id},
        "destination": {"ip": ip_str, "port": port},
        "network": {"transport": TRANSPORT.get(transport, f"unknown({transport})")},
        "sentinel": {
            "schema_version": schema_ver,
            "allowlisted": is_allowed,
            "address_family": "ipv4" if family == 2 else f"unknown({family})",
            "device_timestamp_us": ts_us,
        },
    }

    if not is_allowed:
        print(f"  [ANOMALY] {device_id} -> {ip_str}:{port} "
              f"({event['network']['transport']}) NOT allowlisted",
              file=sys.stderr)

    return event

def parse_stack(device_id: str, schema_ver: int, payload: bytes,
                received_at: str) -> dict | None:
    if len(payload) != STACK_SIZE:
        print(f"[stack] {device_id}: expected {STACK_SIZE} bytes, got {len(payload)}",
              file=sys.stderr)
        return None

    ts_us, wm_words, stack_size, name = struct.unpack(STACK_STRUCT, payload)
    task_name = name.split(b"\x00", 1)[0].decode("utf-8", "replace")

    # Xtensa and RISC-V on ESP32-S3 are 32-bit, word = 4 bytes.
    # On other architectures, collector must know the device's word width.
    wm_bytes = wm_words * 4

    return {
        "@timestamp": received_at,
        "event": {
            "kind": "metric",
            "category": ["host"],
            "action": "stack_watermark",
            "module": "sentinel",
            "dataset": "sentinel.stack",
        },
        "device": {"id": device_id},
        "process": {"name": task_name},
        "sentinel": {
            "schema_version": schema_ver,
            "task_name": task_name,
            "watermark_words": wm_words,
            "watermark_bytes": wm_bytes,
            "stack_size_bytes": stack_size if stack_size else None,
            "device_timestamp_us": ts_us,
        },
    }

def parse_heap(device_id: str, schema_ver: int, payload: bytes,
               received_at: str) -> dict | None:
    if len(payload) != HEAP_SIZE:
        print(f"[heap] {device_id}: expected {HEAP_SIZE} bytes, got {len(payload)}",
              file=sys.stderr)
        return None

    ts_us, free_now, free_min_ever = struct.unpack(HEAP_STRUCT, payload)

    return {
        "@timestamp": received_at,
        "event": {
            "kind": "metric",
            "category": ["host"],
            "action": "heap_metric",
            "module": "sentinel",
            "dataset": "sentinel.heap",
        },
        "device": {"id": device_id},
        "sentinel": {
            "schema_version": schema_ver,
            "free_bytes": free_now,
            "free_min_ever_bytes": free_min_ever,
            "device_timestamp_us": ts_us,
        },
    }
def parse_wifi(device_id: str, schema_ver: int, payload: bytes,
               received_at: str) -> dict | None:
    if len(payload) != WIFI_SIZE:
        print(f"[wifi] {device_id}: expected {WIFI_SIZE} bytes, got {len(payload)}",
              file=sys.stderr)
        return None

    ts_us, sub, channel, reason, bssid_raw, rssi, _reserved = \
        struct.unpack(WIFI_STRUCT, payload)

    bssid = ":".join(f"{b:02x}" for b in bssid_raw)
    sub_str = WIFI_SUB.get(sub, f"unknown({sub})")
    reason_str = WIFI_REASONS.get(reason, f"unknown({reason})")

    event = {
        "@timestamp": received_at,
        "event": {
            "kind": "event",
            "category": ["network"],
            "action": f"wifi_{sub_str}",
            "module": "sentinel",
            "dataset": "sentinel.wifi",
        },
        "device": {"id": device_id},
        "network": {
            "type": "wifi",
            "bssid": bssid,
        },
        "sentinel": {
            "schema_version": schema_ver,
            "sub_event": sub_str,
            "channel": channel if channel else None,
            "rssi_dbm": rssi if sub == 2 else None,
            "reason_code": reason if sub == 2 else None,
            "reason": reason_str if sub == 2 else None,
            "device_timestamp_us": ts_us,
        },
    }

    # Highlight dangerous reason codes
    if sub == 2 and reason in (3, 15, 202, 203, 204):
        print(f"  [WIFI-ALERT] {device_id} disconnected from {bssid}: "
              f"reason={reason_str} ({reason}) rssi={rssi}dBm",
              file=sys.stderr)

    return event

# Registry: adding a new type = +1 line here and +1 parser above.
PARSERS: dict[int, Callable[[str, int, bytes, str], dict | None]] = {
    EVT_BOOT: parse_boot,
    EVT_SOCKET: parse_socket,
    EVT_STACK_WATERMARK: parse_stack,
    EVT_HEAP: parse_heap,
    EVT_WIFI: parse_wifi,
}


# ---------- Dispatcher ----------

def handle_frame(topic: str, payload: bytes) -> None:
    # Parse topic
    parts = topic.split("/")
    if len(parts) < 3:
        print(f"[topic] malformed: {topic}", file=sys.stderr); return
    device_id = parts[2]

    if len(payload) < HEADER_SIZE:
        print(f"[header] {device_id}: frame too short ({len(payload)} bytes)",
              file=sys.stderr); return

    magic, schema_ver, event_type, payload_len = struct.unpack(
        HEADER_STRUCT, payload[:HEADER_SIZE]
    )

    if magic != HEADER_MAGIC:
        print(f"[header] {device_id}: bad magic {magic!r}", file=sys.stderr); return

    body = payload[HEADER_SIZE:]
    if len(body) != payload_len:
        print(f"[header] {device_id}: declared len={payload_len}, got {len(body)}",
              file=sys.stderr); return

    parser = PARSERS.get(event_type)
    if parser is None:
        print(f"[dispatch] {device_id}: unknown event_type={event_type}",
              file=sys.stderr); return

    received_at = datetime.now(timezone.utc).isoformat()
    event = parser(device_id, schema_ver, body, received_at)
    if event is None:
        return

    # print to stdout for testing
    print(json.dumps(event, ensure_ascii=False))
    sys.stdout.flush()
    append_ndjson(event)

def append_ndjson(event: dict) -> None:
    """
    Appends one JSON line to EVENTS_FILE.
    Open/close per event is slow, but safe for concurrent
    readers (Wazuh agent). For MVP at 5-10 events/sec this is more than enough.
    """
    try:
        EVENTS_FILE.parent.mkdir(parents=True, exist_ok=True)
        with EVENTS_FILE.open("a", encoding="utf-8") as f:
            f.write(json.dumps(event, ensure_ascii=False) + "\n")
    except OSError as e:
        # Don't crash due to disk issues — event already sent to stdout.
        print(f"[file-sink] write failed: {e}", file=sys.stderr)

# ---------- MQTT loop ----------

async def run() -> None:
    print(f"[boot] sentinel collector starting, broker {MQTT_HOST}:{MQTT_PORT}, "
          f"topic={TOPIC_FILTER}", file=sys.stderr)

    while True:
        try:
            async with aiomqtt.Client(MQTT_HOST, port=MQTT_PORT) as client:
                await client.subscribe(TOPIC_FILTER, qos=1)
                print("[boot] subscribed", file=sys.stderr)
                async for msg in client.messages:
                    handle_frame(str(msg.topic), bytes(msg.payload))
        except aiomqtt.MqttError as e:
            print(f"[mqtt] disconnected: {e}; retrying in 3s", file=sys.stderr)
            await asyncio.sleep(3)


if __name__ == "__main__":
    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass