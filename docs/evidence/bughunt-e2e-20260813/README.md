# Adversarial E2E — hunting for instability — 2026-08-13

Purpose-built to FIND faults, not confirm health: every scenario here exercises a board path
that had never run on hardware before this run. Full transcript `00-summary.txt`, dev-board
console `01-devboard-console.log`, the harness `bughunt.py` (uses the new `flow put --slow`
hook — 400 ms/chunk, which turns a ~1 s upload into a ~9 s window you can kill things in).

## What was attacked, and what held

| scenario | result |
| --- | --- |
| S1 stale selection (deleted component) | **NAMED refusal** — "component 100152 no longer exists in the graph — update the selection" |
| S2a client killed mid-upload (2304/8000 B staged) | board keeps the partial staging, keeps running, nothing corrupted |
| S2b second upload immediately after (begin-over-begin) | **restarted, not resumed** (received back to 1152 — last-writer-wins, as designed) |
| S2c 65 s of silence | sticky error names it: **"upload abandoned (no data for 60 s)"**, staging dropped |
| S3 clean commit straight after the mess | committed, rebooted, loaded — the FSM carries no residue |
| S4 a put fired into the reboot window of another | **failed gracefully in 6 s** (client-side, board untouched); retry path clean; final state correct |
| S5 whole-run health | **0 panics, 0 watchdogs, 0 unexpected FLOW errors, 0 Modbus mis-pairs** |

Resets captured: 2 (S3 + S4 commits); the console-open reset banner is lost to the serial
open itself — the same capture artifact as every prior run, not a board event.

## The one thing the hunt caught — in the test, not the system

S2a printed `err='-73'`. That is **not** the board's sticky error (which was empty at that
point): the harness's status parser greps `error: (.+)` across the whole client output, and a
client-side zenoh log line (`TLS read error: -73`, the killed session's corpse being cleaned
up on the next connect) matched it. Fourth time this pattern has bitten — a parser that greps
broader than the thing it means to read will eventually report the wrong layer's truth. Noted
here; the board's own `FLOW` lines in the console log are the authoritative record.

## Verdict

Across stale selections, killed uploads, overlapping uploads, idle timeouts, immediate
recommits and mid-reboot collisions: **no state was corrupted, no reboot was unexplained, no
failure was silent.** Combined with the same-day 5-cycle deploy/bus soak (60/60 Q_GOOD,
0 config re-pushes) — the system is stable under everything we knew how to throw at it.
