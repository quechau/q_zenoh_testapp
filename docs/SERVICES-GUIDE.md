# The rubix services, and how to exercise them

What the `.proto` files say, how a request becomes bytes and comes back, and the exact commands
that drive each service against a real board. Every listing here was captured from
`acbm-1cdbd4abbc7c`, not written from the contract.

---

## 1. How the proto files fit together

There are two layers, and keeping them apart explains most of what follows.

```
envelope.proto ............... the transport. Every packet in both directions.
│                              RequestEnvelope / ResponseEnvelope / ErrorInfo
│                              ServiceOperation (OP_READ, OP_WRITE, …), ErrorCode
│
└── payload (bytes) ......... opaque here. Its type is decided by service_id + op.
    │
    ├── service_boardinfo.proto ... system.boardinfo     READ only
    ├── modbus_config.proto ....... modbus.config        READ / WRITE
    ├── modbus_points.proto ....... modbus.points        READ / WRITE / notify
    ├── bacnet_config.proto ....... bacnet.config        READ / WRITE
    ├── bacnet_points.proto ....... bacnet.points        READ / WRITE / notify
    ├── lora_config.proto ......... lora.config          READ / WRITE
    └── lora_points.proto ......... lora.points          READ / WRITE / notify
```

**The envelope does not name the payload's type.** `RequestEnvelope.payload` is `bytes`. A
decoder has to read `service_id` and `op` out of the *same* envelope and look the type up in a
table — which is what [`gen-proto-tables.py`](../scripts/gen-proto-tables.py) generates and
what both this tool and the board's own tracer use.

**The three field buses are deliberately identical past the decode.** `modbus_points.proto`,
`bacnet_points.proto` and `lora_points.proto` are field-for-field the same: `PointValue`,
`PointValues`, `PointReadRequest`, `PointWrite`, `WriteAck`, `Quality`, `WriteReject`. They are
separate packages so each can version independently and so nanopb gets a concrete type per
service — not because a point means anything different. Past the field bus a point is an id, a
number and a quality, and a consumer needs no protocol knowledge to render one.

The **config** protos are where the buses genuinely differ, because that is where a device is
addressed:

| | device is addressed by | a point selects |
|---|---|---|
| `modbus` | `unit_id` (1..247) + `interface` + `baud`/`parity` | `reg_type` + `address` + `data_type` |
| `bacnet` | `mac_addr` (MS/TP MAC 0..254) | `obj_type` + `obj_instance` |
| `lora` | `dev_addr` (32-bit LoRaRaw address) | `field` (temperature, humidity, …) |

Everything else in the config plane is the same shape: `XxxConfig` is the READ snapshot,
`XxxConfigDelta` is the WRITE, and `ConfigResult { applied_ids, rejected[] }` is the verdict.

**Two names are reused across packages.** `PointValue`, `PointWrite`, `ConfigReject`,
`ConfigResult`, `PollClass` and `Quality` each exist three times, once per package. A lookup by
bare name is ambiguous; always qualify — `rubix.embedded.modbus.v1.PointWrite`.

---

## 2. Encode and decode, in the order it happens

```
  CE / test app                                board (ACB-M)

  build payload  (PointWrite, ConfigDelta, …)
        │
  wrap in RequestEnvelope
    1 wire_version   4 seq          7 encoding
    2 service_id     5 client_id    8 payload   ← note: 8
    3 op             6 deadline_ms
        │
  publish → req/<seq>/<board>/svc/<service>
                                   │
                                   ├─ zenoh read task: copy to a queue, nothing else
                                   │
                                   ├─ dispatch task: decode envelope once
                                   │     ADR-016 gate: envelope client_id must equal the
                                   │     session certificate's CN, or ERROR_PERMISSION
                                   │     S1 gate: caller must have logged in, or ERROR_PERMISSION
                                   │     (system.boardinfo is exempt — an unenrolled board
                                   │      must still be discoverable)
                                   │
                                   ├─ service handler decodes the payload with nanopb
                                   │
                                   └─ ResponseEnvelope
                                        1 wire_version  4 seq       7 error
                                        2 service_id    5 encoding  8 is_notification
                                        3 op            6 payload   ← note: 6
                                   │
  ← res/<seq>/<client_id>/<board>/svc/<service>
```

**The envelopes are not symmetric.** A request carries its payload in field **8**; a response in
field **6**. Putting a payload in a request's field 7 lands it on `encoding`, a varint enum —
nanopb fails the whole decode, the board resets the envelope to zeros, and the request arrives
with an empty `client_id` and is refused as a certificate mismatch. The error accuses the wrong
thing entirely. This cost hours; it is why both ends now print the raw bytes.

**A reply carries either a payload or an error, never both.** `error` is an `ErrorInfo`
message, not a number — reading field 7 as a varint finds nothing, which is why a failed login
once printed as two anonymous bytes `08 04` (`ERROR_PERMISSION`).

**Notifications reuse the response envelope** with `is_notification = true`, published on
`cov/<device-ms>/<board>/svc/<service>` rather than an ack key. Nothing requests them; the board
publishes a batch when values change (COV).

### Absent fields are not missing data

proto3 does not transmit default values. A `ModbusPointDef` with `address = 0` sends no address
at all, and a `PointValues` with no entries encodes to **zero bytes** — so a board with nothing
provisioned answers a points READ with no payload whatsoever. That is a correct empty answer,
not a failure. The renderer prints what arrived; it will not invent a field that was not sent.

---

## 3. Setting up

```bash
./scripts/bootstrap.sh              # once: mbedTLS + zenoh-pico with TLS
cmake -S . -B build && cmake --build build -j
```

Identity: `--certs DIR` needs `ca.pem`, `device.pem`, `device-key.pem`. `client_id` defaults to
the CN of `device.pem`, because the board compares the envelope's `client_id` against the
certificate CN on that session and refuses everything on a mismatch.

```bash
./build/q_zenoh_testapp --endpoint tls/192.168.10.29:7447     # interactive
./build/q_zenoh_testapp boardinfo                             # or find the board itself
```

Log in first. Every service except `system.boardinfo` is gated on it:

```
q> login acbm-fabric-2026
  AUTH  UNLOCKED as ce-acf23c0d8637
```

`login` runs `discover` when it has to, because the proof is
`sha256(nonce : client_id : sha256(password))` and **the nonce changes on every board reboot**.
A cached nonce produces a wrong proof that is indistinguishable from a wrong password — both
come back `ERROR_PERMISSION`.

---

## 4. The commands, per service

`<proto>` is `modbus`, `bacnet` or `lora` throughout. Field names and enum values are the
contract's own — a wrong one prints what the `.proto` actually declares rather than guessing.

| Command | Sends | Expects back |
|---|---|---|
| `boardinfo` | `system.boardinfo` READ, empty | `GetBoardInfoResponse` |
| `config <proto> read` | `<proto>.config` READ, empty | `XxxConfig` snapshot |
| `config <proto> add-device f=v …` | `XxxConfigDelta.upsert_devices` | `ConfigResult` |
| `config <proto> add-point f=v …` | `XxxConfigDelta.upsert_points` | `ConfigResult` |
| `config <proto> del-device <id>…` | `remove_device_ids` | `ConfigResult` |
| `config <proto> del-point <id>…` | `remove_point_ids` | `ConfigResult` |
| `points <proto> read [ids…]` | `PointReadRequest` | `PointValues` |
| `points <proto> write f=v …` | `PointWrite` | `WriteAck` |
| `points <proto> sub [secs]` | subscribes to `…/notify` | `PointValues`, `is_notification` |

Every config WRITE is a **delta**. The contract has no replace-all, by design: nothing you send
can clear a board by omission.

### Modbus

```
q> config modbus add-device device_id=1 unit_id=11 interface=IF_RS485_1 enabled=true baud=9600 parity=PAR_NONE
q> config modbus add-point  point_id=101 device_ref=1 reg_type=REG_HOLDING address=0 data_type=DT_U16 scale=1 writable=true poll_class=POLL_NORMAL name=test-point
q> points modbus read
q> points modbus write point_id=101 value=42
```

`baud` is per device, not per port: the board applies baud and parity before each transaction
with that slave, so slaves at different rates can share one RS485 bus, time-multiplexed.
`baud=0` means keep the port's boot default.

> Found by this test: firmware up to and including the `Aug 10 2026` build **accepts** `baud`
> and `parity` and applies them, but omits both from the config READ snapshot. A host doing the
> obvious read-modify-write would silently reset every device it echoed back to the port
> default. Fixed in `zenoh_modbus.c` and verified on hardware — `baud=19200 parity=PAR_ODD` now
> reads back. A board flashed before that fix answers without them.

### BACnet MS/TP

```
q> config bacnet add-device device_id=1 mac_addr=11 enabled=true
q> config bacnet add-point  point_id=201 device_ref=1 obj_type=OBJ_AV obj_instance=1 scale=1 writable=true poll_class=POLL_NORMAL name=relay6
q> points bacnet read
```

There is no discovery — the board's master never sends Who-Is. It synthesises an address-cache
entry from `mac_addr` alone, so a wrong MAC is a permanently faulted point, not an error.

**Writes are permanent.** The master issues WriteProperty at priority 1 and never relinquishes:
the first value written seizes that point's priority-1 slot on the target device until the
target reboots, overriding local logic and any BMS command. Only mark points writable when that
is the intent. Writes to `OBJ_AI`/`OBJ_BI` are rejected outright regardless of the `writable`
flag, because the master decides writability from the object type.

### LoRa (Droplet)

```
q> config lora add-device device_id=1 dev_addr=0x6CC06351 enabled=true
q> config lora add-point  point_id=301 device_ref=1 field=FIELD_TEMPERATURE scale=1 writable=false name=air-temp
q> points lora sub 300
```

The LoRa bus is **push, not poll**. A Droplet broadcasts every few minutes and sleeps; the board
cannot ask it anything. So there is no `poll_class`, a freshly provisioned point stays faulted
until the sensor transmits, and `points lora sub` with a long window is the honest way to watch
it. Every write is rejected `WR_NOT_WRITABLE` until actuator channels land — decided by the
field, the same way BACnet decides by object type.

---

## 5. The script

```bash
./scripts/test-services.sh modbus --endpoint tls/192.168.10.29:7447
./scripts/test-services.sh bacnet --clean
./scripts/test-services.sh lora   --sub-secs 300
```

It runs the whole sequence in dependency order — provision, read back, read values, write,
listen — and prints both views of every packet. Options: `--endpoint`, `--password`,
`--device-id`, `--point-id`, `--sub-secs`, `--clean`.

---

## 6. Reading the answer

```
applied_ids: [1]              accepted
rejected: [{id, reason}]      REJ_BAD_ID (id outside the middleware range), REJ_UNKNOWN_REF
                              (point references a device that does not exist), REJ_POOL_FULL,
                              REJ_IFACE_ROLE (the port is not configured as a master),
                              REJ_DUPLICATE, REJ_INVALID
quality: "Q_GOOD"             the field bus answered
quality: "Q_FAULT"            provisioned, but the device is not answering — wrong unit id /
                              MAC / dev_addr, wrong register, or a line rate the slave does
                              not use
accepted: true                the write reached the device cache. NOT bus-confirmation: the
                              authoritative confirmation is the value coming back on a read
error: { code: … }            the request never reached a handler at all
no payload                    an empty result, which is a valid answer
```

`ERROR_PERMISSION` on a service that worked a minute ago usually means the login lapsed (the
board reboots, the nonce changes, the grant is gone), not that the request is malformed.

---

## 7. Worked example, captured from the board

Provisioning one Modbus device, byte for byte:

```
REQ  req/<seq>/acbm-1cdbd4abbc7c/svc/modbus.config  57B
  json
  { "wire_version": 1, "service_id": "modbus.config", "op": "OP_WRITE", "seq": 58888,
    "client_id": "ce-acf23c0d8637",
    "payload": { "upsert_devices": { "device_id": 1, "unit_id": 11,
                                     "interface": "IF_RS485_1", "enabled": true,
                                     "baud": 9600, "parity": "PAR_NONE" } } }
  encoded (57 B)
    08 01 12 0d 6d 6f 64 62 75 73 2e 63 6f 6e 66 69 67 18 02 20 88 cc 03 2a 0f 63 65 2d 61 63 66 32
    33 63 30 64 38 36 33 37 42 0f 0a 0d 08 01 10 0b 18 01 20 01 28 80 4b 30 01

ACK  seq=58888  30B  round trip 50 ms
  { …, "payload": { "applied_ids": [1] } }
```

Follow the payload bytes: `42 0f` is field 8 (payload), 15 bytes. Inside, `0a 0d` is field 1
(`upsert_devices`), 13 bytes: `08 01` device_id=1, `10 0b` unit_id=11, `18 01`
interface=IF_RS485_1, `20 01` enabled=true, `28 80 4b` baud=9600, `30 01` parity=PAR_NONE.
Every byte accounted for.

Then the point, and reading it back:

```
{ "payload": { "upsert_points": { "point_id": 101, "device_ref": 1,
                                  "reg_type": "REG_HOLDING", "data_type": "DT_U16",
                                  "scale": 1, "writable": true,
                                  "poll_class": "POLL_NORMAL", "name": "test-point" } } }
→ { "applied_ids": [101] }

points modbus read
→ { "values": { "point_id": 101, "quality": "Q_FAULT", "src_ts_ms": 63325650, "seq_no": 1 } }

points modbus write point_id=101 value=42
→ { "point_id": 101, "accepted": true, "applied_value": 42 }

points modbus sub
→ { …, "payload": { "values": { "point_id": 101, "quality": "Q_FAULT", … } },
       "is_notification": true }
```

`Q_FAULT` here is honest: the plumbing works end to end — the board accepted the config, polls
the point, stamps it and publishes COV — but the slave at unit 11 is not answering holding
register 0. `accepted: true` on the write is the device cache accepting it, not the bus
confirming it; the authoritative confirmation would be the value reading back.

Note that `address` does not appear in the point echo above: it was `0`, and proto3 does not
transmit defaults.

---

## 8. Watching it from the board's side

The same two views exist in the firmware. On the board's console:

```
acb-m-riot> login technician 123456      # param_set needs Technician or higher
acb-m-riot> param_set 710 2              # 0 off, 1 JSON, 2 JSON + full hex; live within ~5 s
```

Both ends then print the same packet from their own vantage point, which is the only way to
settle "the board never got it" against "the board ignored it". The board's view of the
`add-point` above:

```
I (175315) ZTR: RX << modbus.config  65 bytes
  encoded (65 B)
    08 01 12 0d 6d 6f 64 62 75 73 2e 63 6f 6e 66 69 67 18 02 20 c2 9d 01 2a 0f 63 65 2d 61 63 66 32
    33 63 30 64 38 36 33 37 42 17 12 15 08 66 10 01 18 03 20 07 28 05 35 cd cc cc 3d 4a 04 66 6c 6f
    77
  json
  {
    "wire_version": 1, "service_id": "modbus.config", "op": "OP_WRITE", "seq": 20162,
    "client_id": "ce-acf23c0d8637",
    "payload": { "upsert_points": { "point_id": 102, "device_ref": 1,
                                    "reg_type": "REG_INPUT", "address": 7,
                                    "data_type": "DT_F32", "scale": 0.1, "name": "flow" } }
  }
```

`35 cd cc cc 3d` is `scale`: field 6, wire type 5, and the four bytes of the float `0.1`. A
`double` would have been eight bytes on wire type 1 — the declared type is what tells the two
ends apart, which is why both read it from the same generated schema rather than guessing.

Tracing prints from the dispatch task, never the zenoh read callbacks, so a slow console cannot
stall reception. It is still UART-speed: level 2 on a 640-byte request is about 2 KB of output,
roughly 170 ms at 115200 baud. That is why it is off by default.


---

## 9. The devices on the bench

The profiles in `test-services.sh` are the hardware from `ACB-M/docs/*-Module/*-test`, not
invented examples. That matters more for Modbus than anywhere else: a wrong register address
still reads *something*, and the reply looks perfectly healthy.

### The address rule, verified

**`address` in the config plane is the PDU address — the console number minus one.**

```
  register map (slave's own numbering)
        │  the console convention differs per slave: the HVAC board support wants +1,
        │  the ZC-Damper wants the number as printed
        ▼
  console:  mb_read_ao 1 11 1000
        │  srvc_modbus subtracts 1
        ▼
  PDU:      999                      ←  this is what `address=` takes
        │  zenoh_modbus.c adds 1 back for MBCTL, which is 1-based
        ▼
  slave:    1000 = Device Alive
```

Proven on hardware: `reg_type=REG_HOLDING address=999` on the ZC-Damper reads **53521**
(`0xD111`), the constant alive value. Anything else means the convention is wrong, not that the
damper is broken — which is exactly why that register is the first thing the profile provisions.

### Modbus — HVAC board support (slave 1, 38400-8-N-1)

| point | `address` | console | meaning |
|---|---|---|---|
| `hvac-power` | 40000 | `40001` | 0/1 |
| `hvac-setpoint` | 40001 | `40002` | **0.1 °C** — 220 is 22.0 |
| `hvac-room-temp` | 40002 | `40003` | read-only |
| `hvac-conn` | 40007 | `40008` | AC attached? |

```bash
./scripts/test-services.sh modbus --device hvac
```

Measured: `hvac-setpoint` read **240** — the 24.0 °C left by the console test of 2026-07-03,
still in place and now readable over zenoh. When `hvac-conn` reads 0 the AC is not attached and
every other HVAC register reads 0 too; that is the slave's state, not a transport fault.

A write needs `writable=true` **at provisioning time**. Without it the board answers
`WR_NOT_WRITABLE` and never touches the bus — the flag is part of the point definition, not of
the write.

### Modbus — ZC-Damper (slave 11, 38400-8-N-1)

| point | `reg_type` | `address` | console | meaning |
|---|---|---|---|---|
| `zcd-alive` | HOLDING | 999 | `1000` | always 53521 |
| `zcd-pos-0` | INPUT | 999 | `1000` | damper 0 position, % |
| `zcd-state-0` | INPUT | 1099 | `1100` | damper 0 state |
| `zcd-target-0` | HOLDING | 42000 | `42001` | damper 0 target, 0..100 |

Holding and input share the same numbers here — `reg_type` is what separates them, and it is
the function code, not a label. After a reboot the board homes all ten dampers sequentially for
2.5–5 minutes before anything responds sensibly, and a stroke takes ~15 s, so a read-back of a
target must wait.

### Modbus — DDM18SD energy meter (9600-8-E-1) — not on the bench

> The meter is not wired up at the moment, so this profile is written from
> `ACB-M/docs/Modbus-Module/ddm18sd-energy-meter-test` and has not been run over zenoh. It is
> kept because it is the case that justifies per-device line settings.

```bash
./scripts/test-services.sh modbus --device ddm18sd
```

**This meter is why per-device `baud` and `parity` exist.** It only speaks 9600-8-E-1 and wrong
parity is total silence, so over the console the whole RS485 port had to be switched to
9600-8-E-1 and back — during which nothing else on the bus could be reached. In the config plane
the line rate travels *with the device* and the board applies it before each transaction, so the
meter and a 38400-8-N-1 ZC-Damper can share one bus, time-multiplexed. That only works if the
board reports what it stored, which is the fix in §4.

Its values are IEEE-754 floats over two registers (high word first), so the points use
`data_type=DT_F32`. It is read-only over this path: changing anything on the meter needs FC
0x10, which the master does not issue. Note it answers to unit 1, the same as the HVAC board
support — the two cannot be wired at once.

### BACnet MS/TP IO card

```bash
./scripts/test-services.sh bacnet --device io-card --mac 11
```

`mac_addr` is the DIP-switch setting on the card. There is no discovery, so a wrong DIP is a
permanently faulted point rather than an error. Writes go out at priority 1 and are never
relinquished — see §4.

---

## 10. Two things measured on Modbus, one of them a defect

### `seq_no` does not mean "the value changed"

The board bumps `PointValue.seq_no` inside a COV gate, so it looks like proof that a value moved:

```c
const bool b_changed = pstru_point->b_dirty_first || (b_ok != pstru_point->b_last_valid) ||
                       (b_ok && (flt_value != pstru_point->flt_last_value));
if (!b_changed) continue;
...
pstru_point->u64_seq_no++;
```

It is not, because of the first term. When a notify publish fails the board calls
`v_ZMB_Mark_All_Dirty()` to re-arm — deliberately, so values are not lost — and that sets
`b_dirty_first` on **every** point. Measured: `seq_no` climbed 3 → 7 across six reads in six
sessions with the value fixed at 53521; four reads inside **one** session left it at 10. It
tracks sessions, not values, because a session ending is what makes the next publish fail.

Use `seq_no` for last-writer-wins ordering, which is what the contract defines it for. Do not
read it as a change indicator.

### CONFIRMED: a failed Modbus read is published as `Q_GOOD` with zero

Quality does not come from whether *this point's* read succeeded. It comes from whether the
*slave* is considered connected:

```c
/* app_modbus/master/point_handler.cpp */
*pflt_value = x_point->flt_value_get;
return (x_point->b_enabled && x_point->b_connected) ? MBCTL_OK : MBCTL_ERR_COMM_FAILED;
```

`flt_value_get` is whatever was last stored — zero if nothing ever was. So a point reports
`Q_GOOD` with a value it has never successfully read, and a consumer cannot tell that zero from
a real one.

Measured on hardware, in a single capture window:

```
console:   E Srvc_Modbus_Master: Address of holding registers returned from slave 1 is invalid   (×24)
over zenoh: point_id 204 quality Q_GOOD   (no value → 0)
            point_id 202 quality Q_GOOD   (no value → 0)
            point_id 205 quality Q_GOOD   (no value → 0)
            point_id 203 quality Q_GOOD   value 1
```

The master is rejecting the slave's responses twenty-four times while three of the four points
report themselves good. The same shape appears before a point's first successful poll: a freshly
provisioned point reads `Q_GOOD` with no value for as long as the register is unreadable.

**BACnet, on the same board and the same contract, gets this right.** Three points provisioned
against a MAC with nothing behind it read `Q_FAULT`, immediately and consistently. Two protocols,
one bus abstraction, and only one of them tells a consumer to disregard the number.

Why it matters more than a fault would: `Q_FAULT` tells a control loop to hold its last good
value or fail safe. `Q_GOOD` with a zero invites it to act — a damper reporting 0 % and healthy
when the register was never read is indistinguishable from a damper genuinely closed.

The fix belongs in the modbus master, not in the contract: quality must reflect the last
transaction for that point, not the reachability of the slave that owns it.

### Provisioning survives a reboot now — opening the console still causes one

**Fixed 2026-08-12 (ADR-024).** This section used to say the opposite, and the measurement below
was taken when it was true. Devices and points are stored in NVS and reloaded at boot, before any
host connects, so a restart no longer clears them and a reconnecting host no longer re-pushes a
config the board already has. §15 covers the fingerprint that makes that check possible.

What still holds: **opening the board's serial port reboots the ESP32-S3** (the open toggles
DTR/RTS). The reset is no longer destructive to configuration, but it still drops the Control
Engine's session, interrupts whatever bus transaction was in flight, and restarts the poll cycle
— so a capture script that attaches the console mid-test still does not observe the run it
meant to.

Measured: a config read immediately after attaching the console returned no payload at all,
with device 11 and its points provisioned less than a minute earlier.

Take the board-side capture in a separate run from the provisioning, or accept that the trace
starts from a blank board.

### The ZC-Damper addresses are right; the device stopped answering

`ACB-M/docs/Modbus-Module/zc-damper-control-test` says of itself that it was derived from source
and never run. It has been run: `ce-edgelink/docs/evidence/e2e-setget-20260717` is a working
wiresheet against this same board and damper, and its three points are exactly the addresses
this guide uses.

| point | reg type | `address` | measured there |
|---|---|---|---|
| `alive` | HOLDING | 999 | `out = 53521`, ok |
| `position-d0` | INPUT | 999 | `out = 100`, ok |
| `target-d0` | HOLDING | 42000 | `out = 100`, ok, `writable = true` |

So `42000` reads back when the damper is answering, and holding 999 and input 999 really are two
different tables at the same PDU address, separated only by the function code.

In this session `address=999` read `53521` cleanly, and later `address=42000` made the master
log `Address of holding registers returned from slave 11 is invalid` on every poll — with a
single point provisioned and nothing else on the bus — and later still the alive register went
quiet too, with RS485-1 unchanged at `38400-8-N-1` and role Modbus master (`0x0250` and `0x0241`
read back from the console).

Given the July evidence, that is the device or the wiring, not the addressing. **An earlier
revision of this section concluded the document's damper half was wrong. It is not** — the
evidence above predates and contradicts that, and it was the reading I should have checked
before writing the conclusion.

What survives, and matters more: through all of it the points kept reporting **`Q_GOOD`**. A
correctly configured port, an unreachable device, and a consumer with no way to tell.

## 11. Telling concurrent calls apart

### What pins what

```
res/<seq>/<client_id>/<board>/svc/<service>
      ───┬───     ───┬───       ────┬────    ─┬─
     which board  which API   which consumer  which call
```

All four now live in the key, so zenoh delivers a reply only to the caller waiting for it. It
did not always: the key used to stop at `<client_id>`, so with several calls in flight to one
service every waiter received every reply and kept the one whose `seq` matched. Correct, but
each reply was delivered N times and filtered N−1 times.

`seq` is still in the envelope, and it is still what makes a reply *yours* —
`RequestEnvelope.seq` echoed back as `ResponseEnvelope.seq`. **The key is an optimisation; the
envelope is the contract.** A consumer that skips the check because the key already matched is
trusting a routing hint with correctness.

**Subscribe with a trailing `**`.** It matches any number of chunks including zero, so
`res/**/<client_id>/*/svc/*` matches boards that append the id and boards that do not —
which is what makes the two ends upgradable in either order. An exact key without it matches
neither shape once the other ships, and the failure is silent: no error, no log, no replies.

So a reply is yours only when the seq matches **and** the `service_id` is the one you asked for.
This tool checks both. The second check is not redundant: the key is subscribed per request, but
a consumer that keeps one long-lived subscription across services can otherwise accept a reply
that carries the right number from the wrong API.

### `seq` is a counter, +1 per call

Each request takes the next number, starting from a random 32-bit value so that restarting the
tool does not replay the numbers the previous run used.

```
REQ  system.auth EXECUTE       seq=2711876008   74B
RES  system.auth EXECUTE       seq=2711876008   27B  round trip 51 ms
REQ  modbus.points READ        seq=2711876009   44B
RES  modbus.points READ        seq=2711876009   27B  round trip 51 ms
```

It used to be the low 16 bits of the millisecond clock, which **repeats every 65.5 seconds** —
two exchanges a minute apart could carry the same number, and two clients started together would
collide immediately. That is exactly the multi-threaded, multi-consumer case where correlation
matters most, so it was the wrong choice there.

### `seq` and `seq_no` are different things

Easy to confuse, and they answer different questions:

| | where | means |
|---|---|---|
| `seq` | `RequestEnvelope` / `ResponseEnvelope` field 4 | the transaction id — which call this is |
| `seq_no` | `PointValue` field 5 | a per-point counter that bumps whenever that point's value changes; the authority for last-writer-wins |

A climbing `seq_no` on a point whose register cannot change is a signal, not noise — see §10.

### Both ends print it

The board's tracer puts the service and seq on its header line, so a request can be paired with
the consumer's own log even when several are talking at once:

```
consumer                                              board

REQ  system.auth EXECUTE      seq=1056147149   74B
                                    I (53968) ZTR: RX << system.auth  seq=1056147149  74 bytes
                                    I (53973) ZTR: TX >> …/system.auth/res/ce-acf23c0d8637
                                                         seq=1056147149  27 bytes
RES  system.auth EXECUTE      seq=1056147149   27B  round trip 352 ms

REQ  modbus.points READ       seq=1056147150   48B
                                    I (54399) ZTR: RX << modbus.points  seq=1056147150  48 bytes
                                    I (54404) ZTR: TX >> …/modbus.points/res/ce-acf23c0d8637
                                                         seq=1056147150  27 bytes
RES  modbus.points READ       seq=1056147150   27B  round trip 150 ms
```

Byte counts match on both sides of every line, which is the cheap check that nothing is being
altered in transit. Note also what the timings say: the board turns a request around in about
**5 ms** (53968 → 53973), while the consumer measures 50–350 ms. The difference is this tool's
own work — it declares the reply subscriber per request and settles before publishing — not the
board being slow. Measure the board with the board's clock.


### Is the key not duplicating the envelope?

It is, and the overlap is not small. Taking the 56-byte `bacnet.points` write above:

```
08 01                                        wire_version    2 B
12 0d 62 61 63 6e 65 74 2e 70 6f 69 6e 74 73 service_id     15 B   ← also in the key
18 02                                        op              2 B
20 b6 9b 8b db 01                            seq             6 B   ← also in the key
2a 0f 63 65 2d 61 63 66 32 33 63 30 64 38 36 client_id      17 B   ← also in the reply key
42 0c 08 b2 02 11 00 …                       payload        14 B
```

`service_id` and `seq` together are **21 of 56 bytes**, and 21 of the 34-byte reply — 62 % of an
ack is fields its key already carries. So the instinct that something is redundant is right
about the bytes. It is wrong about which copy to delete.

**The envelope is the one that has to stand alone**, for reasons that are already load-bearing
in this codebase:

* The Control Engine matches a reply to its request with `scanSeq(bytes…)` — it reads `seq` out
  of the **envelope**, never out of the key. Take `seq` out of the envelope and correlation stops
  working, on the transport we just spent this whole change improving.
* The same envelopes travel over the **WebSocket transport**, where there is no zenoh key at all.
  `websocket_transport.cpp` *synthesises* a key string from the board and service carried in its
  own framing, precisely so the layers above cannot tell the two apart. An envelope that needed
  its key to be complete would not survive that path.
* An envelope gets logged, stored and forwarded on its own. A packet dump, an evidence file, a
  bug report — none of them carry the key unless someone remembered to write it down.

And the key copy earns its place too: it is what lets a caller subscribe to exactly its own
reply, and what makes a log line readable without a decoder.

So the rule stated in §11 is the whole answer: **the key is an optimisation, the envelope is the
contract.** They agree because one of them is routing metadata and the other is the message, and
a consumer that trusts the key instead of checking the envelope has swapped which is which.

The one field that looks most redundant is the least: `client_id` in the request is the caller's
*claim*, and the session certificate's CN is the *fact*. ADR-016 exists to compare them. Remove
the claim and there is nothing left to check against.

### What the change touched

Moving the id into the key is a contract change, so every producer and consumer had to move
together — a consumer left on an exact old key receives nothing and says nothing about it:

| | |
|---|---|
| `ACB-M app_zenoh.c` | publishes `…/res/<client_id>/<seq>` |
| `ce-edgelink zenoh_transport.cpp` | subscribes `res/**/<cid>/*/svc/*` |
| `ce-edgelink zenoh_peer_probe.cpp` | same |
| `ce-mobile keysV1.ts` | `ackAll` gains `/**`; `ack(...)` takes an optional seq; `parseAckKey` returns it |
| `ACB-M poc/zenoh-e2e/zprobe.py` | subscribes with `/**` |
| this tool | subscribes to the exact `…/res/<cid>/<seq>/**` — one call, one key |
| `keyspace.md` | the peer-bus keys were not in the normative grammar at all; now they are |

The consumers went first, all tolerant, then the board. In that order nothing is broken at any
point, in either direction, whichever end is flashed or deployed first.

This tool takes the exact key rather than the tolerant one, because demonstrating the isolation
is the point. Against a board that predates the change it will simply time out — so the timeout
says so, and names the key such a board would have used.


---

## 12. Recipes

Copy-paste, verified against `acbm-1cdbd4abbc7c`. `EP` keeps the endpoint in one place; drop it
entirely and the tool finds the board itself (mDNS for the name, a TCP sweep for the address).

```bash
cd ~/qs/repos-no-5/q_zenoh_testapp
EP="tls/192.168.10.29:7447"
PW="acbm-fabric-2026"
```

### Several commands in one session

One session, one login, commands in order. Use this rather than a loop of one-shot invocations:
each invocation opens and closes a session, and a session ending makes the board's next notify
publish fail, which re-arms COV on every point (§10).

```bash
{
  echo "login $PW"
  echo "config modbus read"
  echo "points modbus read"
  echo "quit"
} | timeout 120 ./build/q_zenoh_testapp --endpoint "$EP"
```

### Provision a point and watch it settle

The question this answers: does the point read its register, or is it reporting a value it has
not fetched yet? `del-point` first makes the recipe repeatable.

```bash
{
  echo "login $PW"
  echo "config modbus del-point 250"
  echo "config modbus add-point point_id=250 device_ref=11 reg_type=REG_HOLDING \
        address=999 data_type=DT_U16 scale=1 poll_class=POLL_FAST name=probe"
  echo "points modbus read 250"
  echo "points modbus read 250"
  echo "quit"
} | timeout 120 ./build/q_zenoh_testapp --endpoint "$EP" 2>&1 \
  | grep -E '"value"|"quality"|"seq_no"' | tr -d ' ' | paste -sd' ' - \
  | sed 's/^/  fresh: /'

sleep 12
printf "login $PW\npoints modbus read 250\nquit\n" \
  | timeout 60 ./build/q_zenoh_testapp --endpoint "$EP" 2>&1 \
  | grep -E '"value"|"quality"|"seq_no"' | tr -d ' ' | paste -sd' ' - \
  | sed 's/^/  after 12s: /'
```

```
  fresh:     "value":53521, "quality":"Q_GOOD", "value":53521, "quality":"Q_GOOD",
  after 12s: "value":53521, "quality":"Q_GOOD", "seq_no":3
```

`grep | tr -d ' ' | paste -sd' '` collapses each reply to one line, which is what makes several
readings comparable at a glance. Drop the pipeline to see the full packets.

### Watch one value across reads

```bash
for i in 1 2 3 4 5 6; do
  printf "login $PW\npoints modbus read 201\nquit\n" \
    | timeout 60 ./build/q_zenoh_testapp --endpoint "$EP" 2>&1 \
    | grep -E '"value"|"seq_no"' | tr -d ' \n' | sed "s/^/  #$i /"; echo
done
```

Note each iteration is its own session. If you are watching `seq_no`, that matters — see §10.

### Only the exchange, not the packets

```bash
... | grep -E "^\[.*(REQ|ACK|AUTH|ERR) "
```

```
  [  6.029s] REQ      system.auth EXECUTE  seq=4000861847  74B
  [  6.079s] ACK      system.auth EXECUTE  seq=4000861847  27B  round trip 51 ms
  [  6.079s] AUTH     UNLOCKED as ce-acf23c0d8637
```

### Turning something off, and checking it went off

```bash
printf "login $PW\npoints modbus write point_id=204 value=0\nquit\n" \
  | timeout 90 ./build/q_zenoh_testapp --endpoint "$EP" 2>&1 | grep -E "accepted|applied_value"
sleep 10
printf "login $PW\npoints modbus read 204 203\nquit\n" \
  | timeout 90 ./build/q_zenoh_testapp --endpoint "$EP" 2>&1 \
  | grep -E 'point_id|"value"|quality' | tr -d ' ' | paste -sd' ' -
```

`accepted: true` is the device cache accepting the write, **not** the bus confirming it. The
read-back is the confirmation, and the two are not the same event — measured on this bench,
turning the AC off read back as off, while turning it back on was accepted and did not take.
When a write is accepted and does not take, the board's console is where the answer is:

```
E Srvc_Modbus_Master: Address of holding registers returned from slave 1 is invalid
```

That line is invisible over zenoh — see §10, where the point kept reporting `Q_GOOD`.

### The whole service, end to end

```bash
./scripts/test-services.sh modbus --device zcd  --endpoint "$EP" --sub-secs 10
./scripts/test-services.sh modbus --device hvac --endpoint "$EP"
./scripts/test-services.sh bacnet --device io-card --mac 11 --endpoint "$EP"
./scripts/test-services.sh lora   --sub-secs 300 --endpoint "$EP"
./scripts/test-services.sh modbus --device zcd --clean       # remove what it created
```

### Both ends of the same exchange

Enable the board's tracer, run a command, and pair the two logs by `seq`. Open the serial port
**once** — opening it a second time toggles DTR/RTS and reboots the ESP32-S3, which looks like a
crash in the middle of your test.

```bash
python3 - <<'PYEOF'
import serial, subprocess, time, threading
ser = serial.Serial('/dev/ttyACM0', 115200, timeout=0.2)
buf, stop = [], False
def rx():
    while not stop:
        d = ser.read(4096)
        if d: buf.append(d.decode('utf-8', 'replace'))
threading.Thread(target=rx, daemon=True).start()
time.sleep(1)
ser.write(b'\r\nlogin technician 123456\r\n'); time.sleep(2)   # param_set needs Technician
ser.write(b'param_set 710 1\r\n'); time.sleep(2)                # 0 off, 1 JSON, 2 JSON+hex
buf.clear(); time.sleep(7)                                       # the level is cached ~5 s
r = subprocess.run(['./build/q_zenoh_testapp', '--endpoint', 'tls/192.168.10.29:7447'],
                   input="login acbm-fabric-2026\nboardinfo\nquit\n",
                   capture_output=True, text=True, timeout=120)
time.sleep(3); stop = True; time.sleep(0.5); ser.close()
open('/tmp/board.txt', 'w').write(''.join(buf))
open('/tmp/client.txt', 'w').write(r.stdout)
PYEOF

grep -E "ZTR: (RX|TX)" /tmp/board.txt
grep -E "(REQ|ACK) " /tmp/client.txt
```

```
I ZTR: RX << req/<seq>/acbm-1cdbd4abbc7c/svc/system.auth                             seq=2178265798   74 bytes
I ZTR: TX >> res/2178265798/ce-acf23c0d8637/acbm-1cdbd4abbc7c/svc/system.auth  seq=2178265798   27 bytes
```

Leave the board quiet again with `param_set 710 0` — the level lives in NVS and survives reboots.

### The Control Engine against the same board

The real consumer, not a test client. It needs the CE's own mTLS identity and its S1 password
file; `run-demo.sh` finds both and refuses to start on the one failure that looks like a wrong
password but is not — the CE deriving its `client_id` from a different NIC than the one its
certificate was issued for (ADR-016 compares the two).

```bash
cd ~/qs/repos-no-5/ce-edgelink
./scripts/preflight.sh            # read-only checks; starts nothing
./scripts/run-demo.sh             # engine + wiresheet + the edgelink extension
tail -f /tmp/edgelink-diag.log
```

What a healthy attach looks like — each line is a different thing working:

```
MDNS discovered peer=acbm-1cdbd4abbc7c -> tls/acbm-1cdbd4abbc7c.local:7447
JOIN   peer=acbm-1cdbd4abbc7c
AUTH   peer=acbm-1cdbd4abbc7c result=OK (unlocked)
SYNC   svc=modbus.config outcome=0 rejected=0 peer=acbm-1cdbd4abbc7c via=zenoh
NOTIFY key=cov/<device-ms>/acbm-1cdbd4abbc7c/svc/modbus.points bytes=39 via=zenoh
```

`AUTH … result=OK` is the one to look for after any change to the keyspace: it means a request
went out and **its reply came back**, so the ack subscription still matches what the board
publishes. A wrong subscription does not produce an error — it produces a board that appears
never to answer, which reads as `alive=1 authed=0` forever.

The CE and this tool can drive the same board at once; they are different consumers with
different `client_id`s and each receives only its own replies.

### Start from nothing

```bash
{
  echo "login $PW"
  echo "config modbus del-point 201 210 211 212 250"
  echo "config modbus del-device 1 2 11"
  echo "config modbus read"                       # expect no payload: an empty config
  echo "quit"
} | timeout 120 ./build/q_zenoh_testapp --endpoint "$EP"
```


---

## 13. Cookbook — the operations, one at a time

Everything below assumes `EP` and `PW` from §12 and a `login` first. Point and device ids are
**yours to choose**: the host owns them, the board only stores them.

### List what a board has

```bash
printf "login $PW\ndevices\nquit\n" | ./build/q_zenoh_testapp --endpoint "$EP"
```

```
  modbus
    device 100154 unit 11   RS485_1    38400 NONE        enabled
      point 100155   HOLDING:42000 U16  w    modbusPoint
    device 100157 unit 1    RS485_1    38400 NONE        enabled
      point 100158   HOLDING:40001 U16  w    modbusPoint
  bacnet
    device 1      MS/TP mac 4                            enabled
      point 306      BO:6               ro   relay6
      point 304      BI:4               ro   input4
  lora
    device 100151 dev_addr 1824547665                    enabled
      point 100152   TEMPERATURE        ro   edgeLoraInput
```

`devices` reads all three config planes and groups every point under the device that owns it,
each protocol in its own terms — a Modbus point by register, a BACnet point by object, a LoRa
point by field. `w`/`ro` is whether you may write it.

**The board is the only authority on this.** What it holds is not what you provisioned earlier —
it is whatever it actually accepted, which differs whenever an entry was rejected or a later sync
replaced it. Since ADR-024 the board also keeps that across a restart, and each plane's line
carries the fingerprint that lets you check it did (§15).

Two devices with the same physical address — two `mac 4` entries, or two Modbus devices on the
same `unit_id` — are accepted by the board and are usually a mistake: they double the traffic to
one device. `devices` shows them plainly next to each other, which is the point of grouping.

The per-plane form is still there when you want the raw reply rather than the summary:

```bash
printf "login $PW\nconfig modbus read\nquit\n" | ./build/q_zenoh_testapp --endpoint "$EP"
```

The reply is the board's own snapshot — every device and every point it currently holds. **No
payload means no points**, not a failure: `ModbusConfig` with nothing in it encodes to zero
bytes. Swap `modbus` for `bacnet` or `lora`; each protocol has its own config plane.

Remember this is RAM: a board restart empties it and the host re-syncs (§10).

### Read values

```bash
points modbus read                 # every point
points modbus read 201             # one point
points modbus read 201 202 204     # several
```

`read` with no ids sends `PointReadRequest{all: true}` rather than an empty payload — "no ids and
not `all`" is a different request from "everything".

### Write one point

```bash
points bacnet write point_id=306 value=0     # relay off
points bacnet write point_id=306 value=1     # relay on
points modbus write point_id=202 value=250   # HVAC setpoint 25.0 °C (units are 0.1 °C)
```

Three things decide whether a write lands:

* the point must have been provisioned `writable=true` — the flag lives in the point definition,
  not in the write, and without it the board answers `WR_NOT_WRITABLE` and never touches the bus;
* the protocol may refuse regardless — BACnet rejects writes to `OBJ_AI`/`OBJ_BI` by object type,
  and every LoRa write is refused until actuator channels land;
* `accepted: true` means the **device cache** took it. The bus confirmation is the value reading
  back, and those are not the same event (§12).

### Provision a device, then its points

A device first — points reference it by `device_ref` and a point naming a device that does not
exist is rejected `REJ_UNKNOWN_REF`.

```bash
# Modbus: a slave on RS485-1, with its own line rate
config modbus add-device device_id=11 unit_id=11 interface=IF_RS485_1 enabled=true \
                         baud=38400 parity=PAR_NONE

# BACnet: an MS/TP MAC on the board's segment. No discovery — a wrong MAC is a faulted point
config bacnet add-device device_id=1 mac_addr=4 enabled=true

# LoRa: the 32-bit address the sensor prints at boot
config lora add-device device_id=1 dev_addr=0x6CC06351 enabled=true
```

Then points. The fields differ per protocol because this is where the buses genuinely differ —
`address` for Modbus, `obj_type`+`obj_instance` for BACnet, `field` for LoRa:

```bash
config modbus add-point point_id=201 device_ref=11 reg_type=REG_HOLDING address=999 \
                        data_type=DT_U16 scale=1 poll_class=POLL_FAST name=zcd-alive

config bacnet add-point point_id=306 device_ref=1 obj_type=OBJ_BO obj_instance=6 \
                        scale=1 writable=true poll_class=POLL_FAST name=relay6

config lora add-point point_id=401 device_ref=1 field=FIELD_TEMPERATURE scale=1 \
                      writable=false name=air-temp
```

Type a field name the `.proto` does not declare and the tool prints the ones it does, with the
legal values for every enum — so the contract is the reference, not this page:

```
ERR   'register_type' is not a field of rubix.embedded.modbus.v1.ModbusPointDef
HINT  rubix.embedded.modbus.v1.ModbusPointDef accepts:
        point_id       uint
        device_ref     uint
        reg_type       enum: REG_UNSPECIFIED REG_COIL REG_DISCRETE REG_INPUT REG_HOLDING
        address        uint
        data_type      enum: DT_UNSPECIFIED DT_U16 DT_I16 DT_U32 DT_I32 DT_F32 DT_BIT
        ...
```

### Change something already provisioned

Send the definition again with the same id. It is an upsert — there is no separate edit — and the
board says which it was:

```
LIFECYCLE point=201 ... unchanged            nothing moved
LIFECYCLE point=201 ... re-provisioned       the definition changed
```

Editing a point, e.g. to make it writable or move its register:

```bash
config modbus add-point point_id=201 device_ref=11 reg_type=REG_HOLDING address=999 \
                        data_type=DT_U16 scale=1 writable=true poll_class=POLL_SLOW name=zcd-alive
```

**Send every field, not just the one you are changing.** The upsert replaces the definition
rather than merging into it, so a field you leave out reverts to its proto3 default — omit
`writable` and the point becomes read-only. Measured on the bench:

```
config bacnet add-point point_id=306 … writable=true …   -> applied_ids [306]
points bacnet write point_id=306 value=1                 -> accepted: true
config bacnet add-point point_id=306 … (no writable) …   -> applied_ids [306]
points bacnet write point_id=306 value=0                 -> reason: WR_NOT_WRITABLE
```

The board accepted the second definition without complaint — it is a valid point, just a
read-only one now. Nothing warns you; the next write is simply refused.

Editing a **device** is the same call, and carries a consequence worth knowing:

```bash
config modbus add-device device_id=11 unit_id=11 interface=IF_RS485_1 enabled=true \
                         baud=19200 parity=PAR_EVEN
```

Changing a device's `unit_id`, `interface`, `baud` or `parity` **invalidates every point under
it**. They are re-provisioned against the new line and their values start again from nothing, so
expect `Q_FAULT` and a zero until the first poll on the new settings completes. Changing only
`enabled` does not.

Reusing a `device_id` for a different physical device is the same operation and the same
consequence — the id is the key of the upsert, so every point whose `device_ref` names it now
addresses the new device. That is how a set of HVAC points once ended up addressing a damper.

### Remove

```bash
config modbus del-point 201 202       # several at once
config modbus del-device 11           # its points go too
```

Removal is by id and takes several in one call. There is no "remove everything" — the contract
has no replace-all, by design, so nothing a consumer sends can clear a board by omission. To
start clean, delete by id:

```bash
{
  echo "login $PW"
  echo "config modbus del-point 201 202 203 204 210 211 212"
  echo "config modbus del-device 1 2 11"
  echo "config modbus read"                  # expect no payload: an empty config
  echo "quit"
} | ./build/q_zenoh_testapp --endpoint "$EP"
```

A board restart no longer clears it (ADR-024) — the board reloads what it had from NVS. Removing
entries is now the only way to empty a board, which is the point: a controller that forgot its
configuration every time the power blinked was never the behaviour anyone wanted.

### Watch values change

```bash
points modbus sub 30        # 30 seconds of the COV stream
```

Nothing requests these — the board publishes when a value changes. Each frame is a
`ResponseEnvelope` with `is_notification: true`, on
`cov/<device-ms>/<board>/svc/<svc>`. A quiet stream means nothing changed, which on a
stable bus is the normal state; it is not a failure to receive.

### Reach a board you cannot address

```bash
./build/q_zenoh_testapp scan            # mDNS for the name, a TCP sweep for the address
./build/q_zenoh_testapp discover 8      # listen for announce beacons — also prints the nonce
```

`discover` is what `login` runs when it needs a nonce, because the proof is
`sha256(nonce : client_id : sha256(password))` and **the nonce changes on every board reboot**.


---

## 14. Turning the logs on and off

Three ends, three switches. The useful thing is that they print the **same exchange** — pair them
by the `txn_<hex>` in the key and you can see a request leave one, arrive at the next, and its
reply come back.

| end | switch | what it adds | where it goes |
| --- | --- | --- | --- |
| `q_zenoh_testapp` | *(always on)* | every packet as bytes and as JSON | stdout |
| `q_zenoh_testapp` | `--debug` / `QZ_DEBUG=1` | zenoh-pico's own DEBUG and INFO — a line per frame and per keep-alive | stdout |
| ACB-M board | `param_set 710 1` | key + JSON of every packet, both directions | the board console |
| ACB-M board | `param_set 710 2` | the same plus the full hex buffer | the board console |
| ce-edgelink | `EDGELINK_DIAG=1` | the decision trace: `JOIN`, `AUTH result=…`, `SYNC`, `NOTIFY`, `WRITE` | `/tmp/edgelink-diag.log` |
| ce-edgelink | `EDGELINK_TRACE=1` | key + JSON of every packet | the same file |
| ce-edgelink | `EDGELINK_TRACE=2` | the same plus the full hex | the same file |

### The board

```
acb-m-riot> login technician 123456      # param_set needs Technician or higher
acb-m-riot> param_set 710 2
acb-m-riot> param_set 710 0              # quiet again
```

Three things to know before you leave it on:

* **It lives in NVS.** The level survives reboots and reflashes; a board you left at 2 last week
  is still at 2.
* **It takes about five seconds.** The level is cached because reading a parameter reaches NVS
  every time, and doing that per packet would cost more than the tracing.
* **It is UART-speed.** Level 2 on a 640-byte request is roughly 2 KB of output, about 170 ms at
  115200. It prints from the dispatch task, never the zenoh read callbacks, so a slow console
  cannot stall packet reception — but it is not free, and that is why it is off by default.

### The Control Engine

```bash
cd ce-edgelink
EDGELINK_DIAG=1 EDGELINK_TRACE=2 ./scripts/run-demo.sh
tail -f /tmp/edgelink-diag.log
```

Both streams share one file on purpose: a packet dump is worth most read next to the decision it
produced.

```
AUTH   peer=acbm-1cdbd4abbc7c result=OK (unlocked)
REQ >> req/txn_40000000/acbm-1cdbd4abbc7c/svc/system.auth   79 bytes
RES << res/txn_40000000/ce-a0ad9f5f8f2f/acbm-1cdbd4abbc7c/svc/system.auth   27 bytes
```

### Reading one exchange across all three

```
client  REQ  req/txn_b440d9e9/acbm-1cdbd4abbc7c/svc/system.auth                      74B
board   REQ << req/txn_b440d9e9/acbm-1cdbd4abbc7c/svc/system.auth                    74 bytes
board   RES >> res/txn_b440d9e9/ce-acf23c0d8637/acbm-1cdbd4abbc7c/svc/system.auth    27 bytes
client  RES  res/txn_b440d9e9/ce-acf23c0d8637/acbm-1cdbd4abbc7c/svc/system.auth      27B  round trip 51 ms
```

Byte counts matching on both sides of every line is the cheap check that nothing is altered in
transit. And the timings say something the client's own number does not: the board turns a
request around in about **5 ms** while the client measures 50–350 ms. The difference is the
client's per-request subscriber setup, not the board being slow.

### Capturing the board's side without breaking the test

**Opening the serial port resets the ESP32-S3** — the open toggles DTR/RTS. Since ADR-024 the
reset no longer wipes the device config: the board reloads it from NVS before any host connects,
and `devices` after a mid-test console attach shows the same tree and the same fingerprint it
showed before. Measured across a hard RTS reset with no host present:

```
cfgstore modbus: restored 116 bytes fp=292bb67d67b8fdb9
modbus config restored: 2 devices, 2 points (all accepted)
```

Open the console **first** anyway. The reset still interrupts whatever transaction was in
flight, still costs the Control Engine its session, and still restarts the poll cycle — it is
just no longer destructive to configuration. §12 has a capture that does it in that order.

If you need to know whether the board restarted without opening the console at all, read
`PointValue.src_ts_ms` — it is the device clock in milliseconds, so it climbing across two reads
means the board stayed up.

---

## 15. Config persistence and the reconcile fingerprint

Boards keep their device and point config across a reboot (ADR-024, contract 0.2.0). Two things
follow, and both change how you test.

**`devices` prints a fingerprint per plane.** It is the board's own summary of the config it has
stored:

```
modbus  292bb67d67b8fdb9
  device 100154 unit 11   RS485_1    38400 NONE        enabled
    point 100155   HOLDING:42000 U16  w    modbusPoint
```

Take it before a reboot, take it after, compare. Equal means the board came back holding the same
config. That is the whole contract — the value is **opaque**: do not parse it, do not recompute
it, do not compare one board's against another's. `(not persisted)` means firmware older than
ADR-024, and then a host must push unconditionally.

**A reconnect to an unchanged board pushes nothing.** The Control Engine reads each plane's
fingerprint on S1 auth and skips the push when both sides are where the last successful sync left
them — the board still reporting the fingerprint recorded then, and the bytes CE would send
matching the bytes it sent. In `EDGELINK_DIAG=1` output:

```
CONFIGHASH peer=acbm-1cdbd4abbc7c Modbus board=292bb67d67b8fdb9 adopted
SYNC peer=acbm-1cdbd4abbc7c Modbus unchanged on both sides — 2 chunk(s) not sent
```

Two conditions, not one. Gating only on the board's fingerprint would stop deploying wiresheet
edits; gating only on CE's own bytes would miss a board someone re-provisioned out of band.

### Testing across a reboot

```bash
./build/q_zenoh_testapp --endpoint tls/<board-ip>:7447 <<'EOF'
login <password>
devices
quit
EOF
# note the three fingerprints, then reset the board (RTS, or power) and repeat
```

A board that comes back with different fingerprints and an empty tree did not persist — check
its console for `cfgstore <plane>: NVS write failed`, which is logged loudly at the moment the
write fails rather than discovered later by a board that came back empty.

### What it does not cover

* **The board is a replica, not a second author.** The wiresheet still owns configuration. The
  board never invents an entry and never pushes one upward.
* **A stale replica is possible.** A board configured, taken out of service and returned later
  holds its old entries. CE sweeps ids its registry does not know on every auth.
* **A snapshot READ has a ceiling.** `ModbusConfig` is declared at 4146 B against a 1536 B
  response payload, so a registry past 26 fully-named Modbus points cannot be returned in one
  reply. The board now answers with `reply too large: N B needed, M B available` instead of
  sending nothing; before, the consumer just saw a timeout. Persistence itself is not affected —
  the NVS blob is sized to the message bound, not to the envelope.

---

## Related

* [`RIOT-DEPLOY-GUIDE.md`](RIOT-DEPLOY-GUIDE.md) — flows on the board: wiring across sheets,
  compile, the Deploy button, testing and troubleshooting
* `control-engine-docs/Contracts/proto-contracts/proto` — the contract this tool builds against
  when a checkout sits beside it (CMake says which source it used). [`../proto/`](../proto) is a
  bundled fallback for standalone builds and can lag — it did, and a board field this tool had
  never heard of printed as absent rather than as an error.
* [`../scripts/gen-proto-tables.py`](../scripts/gen-proto-tables.py) — generates the schema
  tables for this tool and for the firmware
* `ACB-M/components/app_zenoh/zenoh_trace.c` — the board-side tracer
* `ACB-M/docs/Zenoh-Module/` — the transport: two sessions, peer capacity, bring-up
