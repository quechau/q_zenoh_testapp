# Phase 4 — the compiler, accepted on hardware — 2026-08-13

`scripts/riotc.py` compiles a JSON flow description into a `.riot` application. Full run in
`02-acceptance.log`; the flow sources are committed beside it.

## The acceptance, measured with ZERO host processes running

```
host stopped: 0 CE processes
A. slave-1 power register                    reg=0
B. deploy interlock-on  (temp > -5 → 1)      flow: sha=b2d69155… running=yes nodes=3
                                             reg=1     ← the compiled flow wrote it
C. deploy interlock-off (temp > 100 → 0)     flow: sha=7ceefdfd… running=yes nodes=3
                                             reg=0
D. restore factory default                   sha=502a68b3… running=yes
```

`running=yes nodes=3` is the line Phase 3 measured as missing: `loadApp()` **accepted** a
compiled flow — droplet (RP_LORARAWGW) → compare-greater (RP_COMPARE) → modbus-write
(RP_MODBUS_MASTER) — instantiated all three nodes, and RIOT's own Modbus master drove the
physical register on slave 1. Two different compiled flows each moved the register. No
Control Engine, no wiresheet, no host anywhere in the loop. This is the roadmap's headline
sentence, measured: **the logic runs when the host does not.**

(The droplet was radio-silent, so its temperature output sat at the default 0 — which is
exactly what made `0 > -5` vs `0 > 100` a deterministic two-flow test.)

## The two rules, both tested (`01-compile.log`)

* **Determinism** — recompiling the unchanged graph is byte-identical, so
  `sha(compiled) == sha(deployed)` is a stable truth for the Deploy button.
* **Refusal** — unknown node type, nonexistent pin, doubly-driven input, bad enum: each a
  named error, exit 1, nothing emitted. Measured on this bench: `loadApp()` *erases* what it
  dislikes, so a guessed byte would not merely fail — it would silently un-deploy.

## Format facts the compiler encodes (verified against headers + riot_file.md)

Packages `RP_LORARAWGW 0x4F4C` (droplet node 1, output 0 = temperature, 1 datapoint =
8-hex-char address), `RP_COMPARE 0x6F63` (greater 2 / less 3, 2 in / 2 out, 1 datapoint =
float32 default of term 2), `RP_MODBUS_MASTER 0x626D` (write node 2, 8 datapoints: port,
slave, obj_type, addr u16 **1-based**, data_type, endian, timeout u16, refresh u16).
Min-engine-version must be exactly `1.0.0` — the strict rule measured in Phase 3.
