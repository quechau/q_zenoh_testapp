# "Write 1 nhưng đọc về 0" — the dual-writer race, isolated — 2026-08-13

Reported after pressing Deploy: the flow writes 1, the register reads back 0, the AC does not
start; the board looked crashed. Three separate truths untangled, each measured.

## 1. The board never crashed

Port up, `src_ts_ms` climbing, `running=yes nodes=3`. What looked like a crash was the
**designed deploy reboot** (~35 s) — every other card on the sheet goes `fault` with no
explanation while the board reloads. UX item, now in the guide.

## 2. The AC being off was CORRECT control

The operator's threshold at that moment: 30.1. The droplet: 29.86. `lessThan(30.1, 29.86)` =
false → OFF. The droplet hovers around the threshold, so the earlier `in=1` screenshot was a
moment above 30.1. Nothing was broken in that photo but expectations.

## 3. The real defect: two writers, one register

Manual test, RIOT interlock deployed: write 1 → reads `1` then `0,0,0` within ~3 s.
Then the decisive control: **RIOT flow removed** (factory default) → write 1 → **still**
`1, 0, 0, 0`. So the flow was not the only fighter — the **host loop itself** re-asserts its
verdict on every change; and when the RIOT flow is present it re-asserts *its* verdict every
2 s. Two writers, each correct alone:

* host writes the verdict of the **live** threshold (edits apply instantly);
* the board flow writes the verdict of the **deployed** threshold (edits apply on Deploy).

Any DIRTY window — an edited threshold not yet deployed — makes the verdicts differ, the
register oscillates, and the loser reads as a device fault. That is the user's screenshot.

## The fix: the refusal the design already named

`studio2riot` now **refuses** to compile a modbus-write whose `in` is edge-driven on the
sheet (one point, one writer), naming both resolutions: keep the loop on the host and drop
the point from the selection, or accept the race knowingly with `--allow-dual-writer` (a
bench decision, never a production default). Verified: the exact interlock selection now
refuses with that message.

The clean long-term shape stays with the engine (backend item): a `runOn: board` subtree
should stop being **evaluated** by the host, at which point the edge is compile-source only
and the refusal can relax.

Bench state restored: factory default flow on the board, host loop is the single writer, AC
follows the sheet threshold as before.

## Final hop (later the same day): the register never "reverts" — it answers honestly

The operator confirmed the value arrives at the support board's UART, deleted the sheet link
and set the point directly — still "reverted". The slave source (read-only copy under
`sample-sources/ACB-M`) closes the case:

```cpp
READ  POWER → getUartPowerState() → hvac_get_current_state()->power_state  // the AC's ACTUAL state
WRITE POWER → setUartPowerState() → "Set UART power: ON" → hvac_write_power_state(1)  // a COMMAND
```

The register is **command-in / actual-state-out**. The log line the operator saw ("Set UART
power: ON") is the command being accepted and queued; the subsequent reads return the real
machine's state. Measured live via a temporary point on the connection-status register:

```
MBCTL_FGA_UART_CONN_STATUS (internal 40007, RO)  =  0   ← AC not connected
```

So: command accepted → AC absent → actual state stays 0 → every read reports 0. Nothing in
the stack reverts anything; the readback is the truth about the hardware. Fix is physical:
restore the FGA UART link / AC power, confirm reg 40007 reads 1, then POWER writes will hold.
