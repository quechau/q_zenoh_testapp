# Adversarial E2E — hunting for instability — 2026-08-13

Designed to break things, not confirm health: every scenario targets a path that had never
run on hardware. Dev board `acbm-1cdbd4abbc7c`; full console in `01-devboard-console.log`.

| scenario | result |
| --- | --- |
| S1 stale selection (deleted uid) | REFUSED **by name**, never a bare ERROR |
| S2a client killed mid-upload (8 KB, throttled) | board keeps partial (`received=2304`), stays healthy |
| S2b begin-over-begin, no waiting | restarted clean (`received=1152`), last-writer-wins |
| S2c 60 s idle timeout | `error: upload abandoned (no data for 60 s)` — named, FSM freed |
| S3 clean commit straight after the mess | committed, rebooted, verified |
| S4 put fired into another commit's reboot window | failed **gracefully** in 6 s, retry clean |
| S5 whole-run verdicts | **0 panics · 0 watchdog · 0 mis-paired Modbus · 0 unexpected logs**; exactly the 2 commit-reboots expected |

**Verdict: stable.** No board defect found by any adversarial path.

One honest artifact, in the test not the system: the harness' status parser greps `error: (.+)`
across the whole client output and once captured a client-side zenoh log (`-73`, the killed
session's TLS chatter) as if it were `FlowStatus.error`. The board's own field was empty. The
same class of over-greedy parsing produced false verdicts twice before — noted so the next
harness author anchors on the `flow:` block, not the whole transcript.

Test hook added for reproducibility: `flow put <file> --slow` (400 ms/chunk) — a mid-upload
kill becomes a scripted step instead of a race against a ~1 s transfer.
