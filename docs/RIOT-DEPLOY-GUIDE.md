# RIOT deploy — usage & testing guide

Everything the Node-RED-parity roadmap built, in one place: how to wire across sheets, author
a board flow, deploy it, verify it, and test the whole stack the way it was accepted. Design
docs: `control-engine-docs/Decisions/` (`nodered-parity-roadmap.md` carries every phase's
measured answer). Companion: [`SERVICES-GUIDE.md`](SERVICES-GUIDE.md) for the wire services.

Every number in this guide was measured on the bench 2026-08-13 (board `acbm-1cdbd4abbc7c`);
the evidence folders named throughout hold the logs and screenshots.

---

## 1. The pieces and where they live

| piece | where | what it does |
| --- | --- | --- |
| Copy reference / Wire from | wiresheet property-row menu | draw a wire between sheets (an ordinary edge; ghosts render both ends) |
| `riotc.py` | `q_zenoh_testapp/scripts/` | flow JSON → `.riot` binary; deterministic, refuses loudly |
| `studio2riot.py` | `q_zenoh_testapp/scripts/` | LIVE Studio graph (REST) → flow JSON for riotc |
| `deploy.py` | `q_zenoh_testapp/scripts/` | the decision engine: status / deploy / history / readback |
| Deploy button | wiresheet, top-right widget | renders `deploy.py` through the dev-server door (`vite.config.ts riotDeployDoor`) |
| `flow put/status` | `q_zenoh_testapp` REPL | raw `system.flow` access — the debuggable layer under everything |
| board handler | `ACB-M zenoh_flow.c` | chunked upload, sha+header verify, persist-then-ack, reboot to load |

One rule ties them: **state = hash comparison, never memory.** Kill any piece, remount it,
and it must tell the same truth.

---

## 2. Daily usage

### 2.1 Wire across sheets

Right-click the **property row** (not the card header):

1. On the source row (e.g. `compare.lessThan`): **Copy reference**.
2. Navigate to any other sheet; right-click the target row (e.g. `hvac-power.in`):
   **Wire from "lessThan"** — shown only for opposite-direction rows.
3. Both sheets now render the wire with a dashed ghost box for the far end.

Direction is resolved from the row categories (source is always the output side), so copying
either end works. One pending reference at a time. The edge is a plain row in `edges` — the
LoRa→compare→Modbus interlock runs through exactly such an edge (uid 1000022).

Known limits: the built-in "Expose on <folder>" ports work only on plain Folders — device
containers answer *folder not found* (engine item, backend team). Ghost jump-to and a loud
*missing* state are deferred.

### 2.2 Author a flow the board can run

The compiler maps these Studio types (and refuses everything else, by name):

| Studio | RIOT | notes |
| --- | --- | --- |
| `edgeLoraInput` (+parent `edgeLoraDevice`) | droplet | `devAddr` = 8 hex chars; `field` picks the output pin (Temperature→temp) |
| `compare` | compare-greater | `in1` must be a **constant** (becomes term2 default); only `lessThan` may be consumed; measured semantics: Studio `lessThan(in1,in2)` ≡ RIOT `Greater(value:=in2, term2:=in1)` |
| `modbusPoint` (writable, parent `modbusDevice`) | modbus-write | Studio address is 0-based, RIOT is 1-based (**+1 is applied for you**) |

### 2.3 Deploy — the button

The widget top-right polls every 20 s. Its states, verbatim from the CLI:

* **in sync** (green, quiet) — `sha(compiled) == sha(board)`. Nothing to do.
* **dirty** (blue `Deploy ▸`) — your edits are not on the board. Click → dialog shows the
  selection, both shas and the tool's own log → Deploy → wait for the readback → in sync.
* **foreign** (orange `Inspect…`) — the board runs a flow this Studio never deployed.
  The dialog's **Force deploy** overwrites it; that is your decision, never a default.
* **deploying** (violet) — upload + board reboot + readback in progress (~30–45 s).

The selection field in the dialog holds the uids to compile (persisted per browser) until the
graph-side `runOn` property exists.

### 2.4 Deploy — the CLI (works with the UI down)

```bash
cd q_zenoh_testapp
python3 scripts/deploy.py status 100152,100164,100170     # compile + compare, change nothing
python3 scripts/deploy.py deploy 100152,100164,100170     # + upload, readback-verify, record
python3 scripts/deploy.py deploy <uids> --force           # overwrite a FOREIGN flow
```

History: `ce-studio/engine/data/riot-deploy-history.json` — every sha this Studio deployed.
**Rollback** = redeploy any earlier version (compile that state again, or `flow put` a kept
`.riot`). The factory default is `scratchpad`-reproducible: 15 bytes, sha `502a68b3…`.

### 2.5 Raw layer (debugging, hand-written flows)

```bash
./build/q_zenoh_testapp --endpoint tls/<board>:7447
q> login <password>
q> flow status                      # sha / running / node_count / sticky error
q> flow put my.riot                 # begin → 384 B chunks → commit → board reboots in 3 s
q> flow put my.riot --corrupt       # the refusal test: must NOT change the board
```

`python3 …/riot-engine/tools/riot_header.py validate my.riot` checks a file before upload.

---

## 3. Testing

### 3.1 Smoke (5 minutes, no hardware risk)

```bash
cd q_zenoh_testapp
python3 scripts/riotc.py <flow.json> /tmp/a.riot && python3 scripts/riotc.py <flow.json> /tmp/b.riot
cmp /tmp/a.riot /tmp/b.riot                     # MUST be byte-identical (determinism)
python3 scripts/deploy.py status <uids>         # compiles live graph, prints state, changes nothing
```

### 3.2 Refusals (must all fail, by name)

* riotc: unknown node type · nonexistent pin · doubly-driven input · bad enum · >8192 B —
  each exits 1 with the reason (`docs/evidence/phase4-compiler-20260813/01-compile.log`).
* transport: `flow put x.riot --corrupt` → board answers `sha256 mismatch`, old flow keeps
  running, **no reboot** (`ACB-M/docs/evidence/phase3-system-flow-20260813/`).
* deploy: FOREIGN without `--force` → rc 3, nothing sent.

### 3.3 The acceptance loop (what "all good" looks like)

Run `deploy.py deploy`, then **stop the Control Engine entirely** and check with the test
client: `flow status` shows the compiled sha, `running=yes nodes=3`, and the flow's output
register still answers (`points modbus read`). That sentence — *the logic runs when the host
does not* — is the whole point; if it fails, nothing else matters.

### 3.4 Resilience (run after risky changes)

`ce-edgelink/docs/evidence/resilience-e2e-20260813/` is the template: reboot the CE (expect
one full push — a fresh process has no syncedHost), reboot the board with the CE untold
(expect **0** config writes, fingerprints identical, cards re-seeded, flow sha unchanged).
Count on the **board's console** — it is the only witness that cannot be talked out of it.

### 3.5 Evidence discipline (the process that keeps paying)

Per phase: a folder under `docs/evidence/<phase>-<date>/` with the raw logs (both sides),
screenshots for anything UI, a README that states what was measured — **including what went
wrong**. This session's verification runs caught, by themselves: the sticky seed flag, the
`IN SYNC`→`IN` regex, a foreign demo that silently no-oped, and a verdict script whose own
counter lied. Write those down too; a verdict that fails for the wrong reason and ships anyway
is how confidence rots.

---

## 3.6 When a deploy fails — the recovery ladder

Every failure mode leaves the board in a **known** state; work down the ladder, never guess:

| where it failed | board state | what to do |
| --- | --- | --- |
| compile refused (riotc / studio2riot) | untouched — nothing was sent | fix the **named** error; that is what the names are for |
| upload/commit refused (`sha256 mismatch`, bounds, incomplete) | **old flow still running** | re-run deploy; chunks are idempotent, a retry is safe |
| commit OK but readback shows the **old/default** sha | `loadApp()` rejected it at boot and **erased** it — board fell back safely | the flow is invalid for this firmware: unregistered package or min-version ≠ 1.0.0; fix and redeploy |
| readback timeout | board mid-reboot (~35 s) | `deploy.py` polls 120 s; if it still times out, run `deploy.py status` again — never assume |
| board unreachable | rebooting or network | wait for port 7447, retry; nothing is half-applied (persist-then-ack) |
| everything else | — | `flow put default.riot` restores the factory flow; device config is untouched (ADR-024); FC 0x15 remains the field-recovery door |

The one thing you never need to do is wonder whether a failed deploy half-applied: the commit
verifies before anything swaps, persists before it acks, and a rejected flow is erased by the
engine itself with the default restored.

---

## 4. Troubleshooting — every entry happened for real

| symptom | cause | fix / where proven |
| --- | --- | --- |
| value `0` missing from JSON / parser says point absent | proto3 omits defaults — absent **means** 0 | parse absent-as-zero (SERVICES-GUIDE §15; bacnet 4/8→8/8 incident) |
| card blank `status ''`, board says `Q_GOOD seq_no=1` | the one COV fired before auth; static value never fires again | fixed: seed **per auth**; if seen again check `SEED point=` in diag |
| deploy compiles a stale threshold | values read from `data.db` — it is the auto-commit snapshot (held 29.6 vs live 34.1) | `studio2riot` reads REST only; never build on the DB |
| board comes back running the **old** flow after commit OK | `loadApp()` rejected it at boot and **erased** it (fallback to default) | unregistered package or min-version ≠ `1.0.0`; deploy.py readback catches this |
| `Expose on <device>` → banner *folder not found* | engine pins ports on plain Folders only | use Copy reference / Wire from; engine item stays with backend |
| Deploy button stuck DIRTY forever after deploy | state pipe mangling (the `IN SYNC`→`IN` regex was exactly this) | check `/riot-deploy/status` JSON directly |
| widget FOREIGN unexpectedly | board sha not in this Studio's history — someone else deployed | Inspect first; `--force` only as a decision |
| testapp shows TLS `-76`/write errors during `flow put` | board mid-reboot from a previous commit | wait for port 7447, retry; the upload is idempotent by offset |
| opening the serial console "breaks" the test | opening the port **resets the ESP32-S3** | open console first; config+flow survive (ADR-024) but sessions/polls restart |
| PATCH returns `000` | engine still booting (a stale diag line can fool a wait-for-auth grep) | wait on fresh log content, or on an actual REST read |
| register writes rejected at exact value | device-side rule (damper accepts steps of 5) | test with values the device accepts |
| write 1, ack `applied=1`, next read already 0 (immediate) | POWER is **command-in / actual-state-out**: WRITE sends a command to the AC, READ returns the AC's real state — an absent AC reads 0 forever (source: `modbus_handlers.cpp`) | read reg **40007** (CONN_STATUS): 0 = AC not connected → fix the FGA UART link/AC power; 1 = investigate further |
| write 1, reads back 0 within seconds; AC won't start | **two writers on one register**: host loop re-asserts its live verdict, board flow re-asserts its deployed verdict every 2 s — any DIRTY window makes them differ | one point, one writer; `studio2riot` now refuses such selections (`docs/evidence/dual-writer-20260813/`) |
| every card flashes `fault` ~35 s after Deploy | the designed deploy **reboot** — not a crash | wait for readback; `src_ts_ms` climbing proves no crash loop |
| Modbus replies swap between two points | reply matched by address only across register kinds | fixed by the reply-kind guard; a restart clears residue |
| board console floods TLS `-76`, watchdog fires | fixed sentinel bug (spin on dead socket) — should not recur | if seen: `e2e-after-tls-fix-20260813` is the reference picture |

---

## 5. The rules underneath (do not trade these away)

1. **Deterministic compile** — same graph, same bytes. Emission is uid-ordered everywhere;
   the Deploy button's truth is `sha(compiled)==sha(deployed)` and an iteration-order compiler
   makes the dirty flag lie forever.
2. **Refuse, never guess** — `loadApp()` *erases* what it dislikes, so a guessed byte does not
   merely fail: it silently un-deploys.
3. **Persist, then ack** — the commit OK means the flow survives a power cut, or it is not
   sent. Same ADR-024 discipline as device config.
4. **Foreign is a human decision** — history makes it detectable; `--force` makes it explicit.
5. **Verdicts come from readback and the board's console** — never from the tool's own
   optimism. Twice this week the test lied before the system did.
