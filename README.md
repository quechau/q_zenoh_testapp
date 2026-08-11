# q_zenoh_testapp

A zenoh **peer** in C that speaks the rubix contract, for exercising ACB-M boards by hand.

It is deliberately the same shape as the thing it tests: zenoh-pico 1.9.0, mutual TLS, the
same protobuf envelopes, the same key expressions. What it proves about a board is therefore
what a real consumer would experience — not what a differently-built client happens to do.

```
q> discover
  acbm-1cdbd4abbc7c    7 announces   nonce 2d1aebe04a842687579d0a34b2d08656
q> login <password>
  ACK  seq=20840  25B  round trip 51 ms
        error          = <0 B>
q> boardinfo
  ACK  seq=21191  172B  round trip 101 ms
        payload        = <141 B> 0a 8a 01 0a 05 41 43 42 2d 4d …
```

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

It also builds zenoh-pico with `ZENOH_DEBUG=1`. zenoh-pico only logs at compile time, and
without it a failed handshake is an unexplained return code.

## Use

```bash
# one-shot
./build/q_zenoh_testapp --endpoint tls/192.168.10.29:7447 discover 8
./build/q_zenoh_testapp --endpoint tls/192.168.10.29:7447 boardinfo
./build/q_zenoh_testapp --endpoint tls/192.168.10.29:7447 login <password>

# interactive — one session, state kept between commands
./build/q_zenoh_testapp --endpoint tls/192.168.10.29:7447
q> discover
q> login <password>
q> sub rubix/** 20
q> req modbus.points read
```

| Command | What it does |
| --- | --- |
| `discover [secs]` | listens on `rubix/peers/*/announce`, lists boards **and their nonce** |
| `use <board-id>` | choose which board later commands address |
| `connect [endpoint]` / `disconnect` | open or close the mTLS peer session |
| `login <password>` | S1 login, `sha256(nonce:client_id:sha256(password))` |
| `logout` | `system.auth` with an empty payload |
| `sub <keyexpr> [secs]` | subscribe and decode every envelope that arrives |
| `pub <keyexpr> <text>` | publish a raw payload |
| `req <service> [op]` | protobuf request/reply; `op` = read, write, execute, ping, … |
| `boardinfo`, `points` | shorthands for the two common reads |
| `enroll <id> [SAN]` | get a certificate from the CA and write it into `--certs` |
| `status`, `help`, `quit` | |

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

**The login nonce is per boot.** It is carried in the announce beacon, which is why `login`
runs `discover` first if it has to. A nonce cached across a board reboot silently produces a
wrong proof.

## Layout

```
src/main.c      argument handling, one-shot commands, REPL
src/session.c   zenoh session, discovery, pub/sub, login, request/reply
src/proto.c     protobuf encode/decode for the rubix envelopes (no protoc needed)
src/ca.c        CA enrolment over HTTPS: register, key, CSR, sign
src/util.c      hashing, base64, certificate CN
proto/          envelope.proto, for reference
scripts/        bootstrap.sh
```

## Related

* `ACB-M/docs/Zenoh-Module/` — the firmware side, including the two-session design, the
  manual test walkthrough and the peer capacity measurements
* `ACB-M/poc/zenoh-e2e/` — the Python equivalents of these tests
