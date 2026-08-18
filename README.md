# q_zenoh_testapp

A zenoh **peer** in C that speaks the CE contract, for exercising ACB-M boards by hand.

It is deliberately the same shape as the thing it tests: zenoh-pico 1.9.0, mutual TLS, the
same protobuf envelopes, the same key expressions. What it proves about a board is therefore
what a real consumer would experience — not what a differently-built client happens to do.

```
q> login <password>
  REQ  req/<seq>/acbm-1cdbd4abbc7c/svc/system.auth  72B  seq=34167
        json
        { "wire_version": 1, "service_id": "system.auth", "op": 5, "seq": 34167,
          "client_id": "ce-acf23c0d8637", "payload": "d1a7d9c0…95a7faca" }
        encoded (72 B)
          08 01 12 0b 73 79 73 74 65 6d 2e 61 75 74 68 18 05 20 f7 8a 02 2a 0f 63 65 …
  ACK  seq=34167  27B  round trip 51 ms
        encoded (27 B)
          08 01 12 0b 73 79 73 74 65 6d 2e 61 75 74 68 18 05 20 be cc 01 28 01 3a 02 08 04
        json
        { …, "error": { "code": "ERROR_PERMISSION" } }
  ACK   FAILED — ERROR_PERMISSION (code 4)
  AUTH  DENIED. Either the password is wrong, or the nonce is stale …
```

Every packet is shown in both forms, ordered the way that direction actually happens: a
request is composed as JSON and then encoded; a reply arrives encoded and is then decoded.
The hex is the only record of what really went on the wire — a decoder can be wrong, and one
here was, silently writing a payload into the field a request reserves for `encoding`. The
JSON is what makes 172 bytes of nested protobuf mean something.

**Byte fields print in full.** No previews, no `…`. The bytes a preview cuts are reliably the
ones that turn out to matter.

**A failed reply says why.** `ResponseEnvelope` field 7 is an `ErrorInfo`, not a number, so
reading it as a varint finds nothing and the failure prints as two anonymous bytes. It is
decoded and its code named, and commands report a verdict — `UNLOCKED` or `DENIED` with the
three things that actually cause it — instead of describing how to interpret one. An earlier
version printed "a zero error code above means unlocked" whatever came back, so a refused
login looked exactly like a successful one.

**The schema comes from the contract, not from guesswork.** `scripts/gen-proto-tables.py`
turns `proto/` into C lookup tables at configure time, so fields carry their declared names
and types: enums print as `"OP_EXECUTE"`, `bool` as `true`, `double` as a number, repeated
fields as JSON arrays. Inference was wrong in ways that looked right — a hand-written table
labelled `SupportedService.service_id` as `id` and left `role` as a bare `3` for weeks.
Payloads are routed by (service, op, direction), and the ones that are not protobuf are named
rather than forced through a decoder: `system.auth` carries a bare 32-byte proof, and an empty
payload is a logout. Where the contract does not say, the renderer falls back to inference and
keeps field numbers, so it never invents a label it cannot justify.

## Build

```bash
./scripts/bootstrap.sh          # ~4 minutes the first time, then a no-op
cmake -S . -B build && cmake --build build -j
./build/q_zenoh_testapp --help
```

`bootstrap.sh` fetches and builds Mbed TLS and zenoh-pico into `third_party/prefix`. Nothing
is installed system-wide and no root is needed. Two reasons it has to exist:

* zenoh-pico ships with `Z_FEATURE_LINK_TLS 0`. A packaged build cannot do mTLS at all, so it
  is rebuilt here with TLS on.
* Its CMake finds Mbed TLS through pkg-config rather than fetching it, and most distributions
  do not install the Mbed TLS headers.

It builds zenoh-pico with `ZENOH_DEBUG=3`, the most verbose level. zenoh-pico picks its log
level at compile time, so a quiet build could never be made talkative when something goes
wrong; instead it is built loud and `src/logfilter.c` drops the DEBUG and INFO lines at run
time unless `--debug` is given.

## Use

`--endpoint` is optional. With none, any command that needs a session finds the board first:

```bash
./build/q_zenoh_testapp boardinfo
#   SCAN    no endpoint set — looking for boards on the LAN first
#   SCAN    mDNS named: acbm-1cdbd4abbc7c
#   SELECT  endpoint=tls/192.168.10.29:7447  board=acbm-1cdbd4abbc7c
#   ACK     seq=29344  172B  round trip 51 ms
```

```bash
# one-shot
./build/q_zenoh_testapp scan
./build/q_zenoh_testapp --endpoint tls/192.168.10.29:7447 discover 8
./build/q_zenoh_testapp --endpoint tls/192.168.10.29:7447 boardinfo
./build/q_zenoh_testapp --endpoint tls/192.168.10.29:7447 login <password>

# interactive — one session, state kept between commands
./build/q_zenoh_testapp --endpoint tls/192.168.10.29:7447
q> discover
q> login <password>
q> sub ce/** 20
q> req modbus.points read
```

| Command | What it does |
| --- | --- |
| `scan [secs]` | finds boards with no session and no address: mDNS for the name, a TCP sweep for the address |
| `discover [secs]` | listens on `ce/peers/*/announce`, lists boards **and their nonce** |
| `use <board-id>` | choose which board later commands address |
| `connect [endpoint]` / `disconnect` | open or close the mTLS peer session |
| `login <password>` | S1 login, `sha256(nonce:client_id:sha256(password))` |
| `logout` | `system.auth` with an empty payload |
| `sub <keyexpr> [secs]` | subscribe and decode every envelope that arrives |
| `pub <keyexpr> <text>` | publish a raw payload |
| `points <proto> read\|write\|sub` | the data plane: values, one write, the COV stream |
| `config <proto> read\|add-device\|add-point\|del-device\|del-point` | the control plane, always a delta |
| `req <service> [op]` | protobuf request/reply; `op` = read, write, execute, ping, … |
| `boardinfo`, `points` | shorthands for the two common reads |
| `enroll <id> [SAN]` | get a certificate from the CA and write it into `--certs` |
| `status`, `help`, `quit` | |

`--debug` (or `QZ_DEBUG=1`) adds zenoh-pico's own DEBUG and INFO lines — a line per frame and
per keep-alive. It is off by default because it buries everything else and mangles the REPL
prompt. Its WARN and ERROR lines are never hidden: their absence is what makes a failed
handshake look like an unexplained return code.

### Identity

`--certs DIR` must hold `ca.pem`, `device.pem`, `device-key.pem`. The `client_id` defaults to
the **CN of `device.pem`**, because ADR-016 has the board compare the envelope `client_id`
against the certificate CN on that session and refuse everything on a mismatch. The app warns
at startup if you override `--client-id` with something that does not match — that mismatch is
the most common reason a consumer is refused every service while looking perfectly connected.

To create an identity:

```bash
CA_ADMIN_SECRET=<admin-secret> \
  ./build/q_zenoh_testapp --certs ~/.config/acb-provisioner/certs/my-id enroll my-id
```

Add a SAN when something will **dial** this peer by address, e.g. `IP:192.168.10.39`. The
hackline CA copies SANs from the CSR, and without a matching one the dialler's handshake fails
with `-0x2700` (`MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`) — which looks like a transport fault
because the TCP connect succeeds first.

## Things this tool exists to make visible

**A board that accepts your connection may still ignore you.** zenoh-pico compiles a fixed
`Z_LISTEN_MAX_CONNECTION_NB` (10), shared between inbound peers and outbound links. Past the
ceiling the board completes TCP and TLS, the session reports itself open, and no data ever
arrives. Measured on hardware: 9 peers served with no outbound links, 6 served while 3
outbound links were up. When the board cannot even set up the TLS session it says so on its
console — `_z_tls_accept: Failed to setup SSL: -0x008d` (`MBEDTLS_ERR_SSL_ALLOC_FAILED`) —
and the client only sees `-0x004C` (`MBEDTLS_ERR_NET_RECV_FAILED`).

**Request and response envelopes are not symmetric.** In `RequestEnvelope` the payload is
field **8** and `encoding` is 7; in `ResponseEnvelope` the payload is field **6**. Putting a
payload in field 7 of a request lands it on `encoding`, a varint enum — nanopb fails the whole
decode, the board resets the envelope to zeros, and the request arrives with an empty
`client_id` and is refused as a certificate mismatch. The error you see accuses the wrong
thing entirely.

**Discovery cannot trust who sent an mDNS reply.** Replies are multicast and other hosts
re-announce records they have cached, so the sender is often not the owner — measured here, a
reply carrying the board's records came from a different machine, and dialling that machine
got connection refused. `scan` therefore takes only the *name* from mDNS and finds the
*address* by sweeping the local subnets for something listening on 7447. It sweeps every
interface, not the first one: the board is regularly not on the interface that sorts first,
which is the same trap that makes the Control Engine adopt the wrong identity when an Ethernet
cable is plugged into a WiFi host.

**The login nonce is per boot.** It is carried in the announce beacon, which is why `login`
runs `discover` first if it has to. A nonce cached across a board reboot silently produces a
wrong proof.

## Layout

```
src/main.c      argument handling, one-shot commands, REPL
src/session.c   zenoh session, discovery, pub/sub, login, request/reply
src/proto.c     protobuf encode/decode for the CE envelopes (no protoc needed)
src/ca.c        CA enrolment over HTTPS: register, key, CSR, sign
src/mdns.c      finding boards: mDNS for the name, TCP sweep for the address
src/json.c       protobuf rendered as readable JSON, inferred rather than generated
src/logfilter.c keeps zenoh-pico's chatter out of the way unless --debug
src/util.c      hashing, base64, certificate CN
```

## Related

* [docs/SERVICES-GUIDE.md](docs/SERVICES-GUIDE.md) — how the proto files relate, what each
  service carries, the request/reply sequence, and the commands that drive modbus, bacnet and
  lora end to end
* `ACB-M/docs/Zenoh-Module/` — the firmware side, including the two-session design, the
  manual test walkthrough and the peer capacity measurements
* `ACB-M/poc/zenoh-e2e/` — the Python equivalents of these tests
* `ACB-M/components/app_zenoh/zenoh_trace.c` — the same two views on the board itself, from
  the same generated tables (`param_set 710 2`)
