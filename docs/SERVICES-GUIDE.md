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
  publish → rubix/<board>/svc/<service>/req
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
  ← rubix/<board>/svc/<service>/ack/<client_id>
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
`rubix/<board>/svc/<service>/notify` rather than an ack key. Nothing requests them; the board
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
REQ  rubix/acbm-1cdbd4abbc7c/svc/modbus.config/req  57B
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

### Modbus — DDM18SD energy meter (9600-8-E-1)

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

## 10. `seq_no` does not mean "the value changed"

`PointValue.seq_no` is documented as a per-point monotonic counter and the authority for
last-writer-wins, and the board does bump it only inside a COV gate:

```c
const bool b_changed = pstru_point->b_dirty_first || (b_ok != pstru_point->b_last_valid) ||
                       (b_ok && (flt_value != pstru_point->flt_last_value));
if (!b_changed) continue;
...
pstru_point->u64_seq_no++;
```

So it looks like proof that a value moved. It is not, because of the first term. When a notify
publish fails the board calls `v_ZMB_Mark_All_Dirty()` to re-arm — deliberately, so values are
not silently lost — and that sets `b_dirty_first` on **every** point. The next tick therefore
counts every point as changed and bumps every `seq_no`, with nothing on the field bus having
moved at all.

Measured: `zcd-alive` is the ZC-Damper's constant `0xD111`. Across six reads in six sessions its
`seq_no` climbed 3 → 7 while the value never left 53521. Four reads inside **one** session left
it at 10. It tracks sessions, not values, because a session ending is what makes the next publish
fail.

**For a consumer this means:** use `seq_no` for last-writer-wins ordering, which is what the
contract defines it for. Do not use it as a change indicator, and do not derive a rate of change
from it. A climbing `seq_no` on a value that cannot change is expected here, not a fault.

### An observation that did not survive re-testing

An earlier revision of this section reported a defect: several points reading `Q_GOOD` with an
absent value (zero) on registers that were known-good, which looked like a mis-paired Modbus
response being published as good rather than dropped. That claim rested on the `seq_no`
reasoning above, and the reasoning was wrong.

The zero readings were real and are recorded here rather than deleted — they happened while the
device definition was being re-upserted repeatedly with different line rates (19200/PAR_ODD,
then 9600/PAR_EVEN, then 38400/PAR_NONE), and changing a device's line settings invalidates
every point provisioned under it. With the device left alone at the ZC-Damper's actual
38400-8-N-1, none of it reproduces: a freshly provisioned point reads 53521 immediately, four
points on one device all read plausible values, and forty seconds of idle produce no phantom COV
events on the console.

So: not a confirmed defect, and not evidence of one. What it does illustrate is that a wrong
line rate does not necessarily present as `Q_FAULT` — worth knowing when a point looks healthy
and reads nonsense.

## 11. Telling concurrent calls apart

### What pins what

```
rubix/<board>/svc/<service>/ack/<client_id>/<seq>
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
`rubix/*/svc/*/ack/<client_id>/**` matches boards that append the id and boards that do not —
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
ACK  system.auth EXECUTE       seq=2711876008   27B  round trip 51 ms
REQ  modbus.points READ        seq=2711876009   44B
ACK  modbus.points READ        seq=2711876009   27B  round trip 51 ms
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
                                    I (53973) ZTR: TX >> …/system.auth/ack/ce-acf23c0d8637
                                                         seq=1056147149  27 bytes
ACK  system.auth EXECUTE      seq=1056147149   27B  round trip 352 ms

REQ  modbus.points READ       seq=1056147150   48B
                                    I (54399) ZTR: RX << modbus.points  seq=1056147150  48 bytes
                                    I (54404) ZTR: TX >> …/modbus.points/ack/ce-acf23c0d8637
                                                         seq=1056147150  27 bytes
ACK  modbus.points READ       seq=1056147150   27B  round trip 150 ms
```

Byte counts match on both sides of every line, which is the cheap check that nothing is being
altered in transit. Note also what the timings say: the board turns a request around in about
**5 ms** (53968 → 53973), while the consumer measures 50–350 ms. The difference is this tool's
own work — it declares the ack subscriber per request and settles before publishing — not the
board being slow. Measure the board with the board's clock.

### What the change touched

Moving the id into the key is a contract change, so every producer and consumer had to move
together — a consumer left on an exact old key receives nothing and says nothing about it:

| | |
|---|---|
| `ACB-M app_zenoh.c` | publishes `…/ack/<client_id>/<seq>` |
| `ce-edgelink zenoh_transport.cpp` | subscribes `rubix/*/svc/*/ack/<cid>/**` |
| `ce-edgelink zenoh_peer_probe.cpp` | same |
| `ce-mobile keysV1.ts` | `ackAll` gains `/**`; `ack(...)` takes an optional seq; `parseAckKey` returns it |
| `ACB-M poc/zenoh-e2e/zprobe.py` | subscribes with `/**` |
| this tool | subscribes to the exact `…/ack/<cid>/<seq>/**` — one call, one key |
| `keyspace.md` | the peer-bus keys were not in the normative grammar at all; now they are |

The consumers went first, all tolerant, then the board. In that order nothing is broken at any
point, in either direction, whichever end is flashed or deployed first.

This tool takes the exact key rather than the tolerant one, because demonstrating the isolation
is the point. Against a board that predates the change it will simply time out — so the timeout
says so, and names the key such a board would have used.

---

## Related

* [`../proto/`](../proto) — the contract, copied from `control-engine-docs/Contracts/proto-contracts`
* [`../scripts/gen-proto-tables.py`](../scripts/gen-proto-tables.py) — generates the schema
  tables for this tool and for the firmware
* `ACB-M/components/app_zenoh/zenoh_trace.c` — the board-side tracer
* `ACB-M/docs/Zenoh-Module/` — the transport: two sessions, peer capacity, bring-up
