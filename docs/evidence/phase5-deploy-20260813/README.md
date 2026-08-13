# Phase 5 — Deploy, the decision engine — 2026-08-13

The Deploy button's whole brain, CLI-first: `scripts/studio2riot.py` (the LIVE Studio graph
over REST → flow description) + `scripts/deploy.py` (fingerprint states → upload → readback →
history). The UI button renders exactly this and must never know more than this does. Full
runs: `01-acceptance.log`, `02-foreign-rerun.log`; deploy history `03-history.json`; the
Studio graph it compiles `04-studio-graph.png`.

## The Node-RED loop, measured on the real graph

The selection is the damper interlock as it actually exists in Studio — including edge
1000022, the cross-sheet wire created through the Phase 1 UI:

```
A  status                       compiled=e43d060b… board=502a68b3…(factory)   DIRTY
B  deploy #1                    committed → readback VERIFIED, running=yes nodes=3, history 1
C  Studio edit  in1 34.1 → -5   compiled=b2d69155… ≠ board=e43d060b…          DIRTY
D  deploy #2                    committed → readback VERIFIED, history 2
E  STOP THE HOST (0 CE procs)   flow sha=b2d69155… running=yes nodes=3
                                point 100170 value=1 Q_GOOD   ← the edit, live, no host
F  foreign upload (7ceefdfd…)   state=FOREIGN — "inspect first, or pass --force"
   deploy without --force       rc=3, refused
G  restore                      factory default 502a68b3… running, threshold back to 34.1
```

E is the roadmap's closing sentence with the *real* wiresheet as the source: an operator's
edit (C) reached the field through compile→deploy (D) and kept acting after the host died (E).

## What each state rests on

* **DIRTY vs FOREIGN is history, not guesswork** — `riot-deploy-history.json` beside the
  graph records every sha this Studio deployed; a board sha outside it is someone's work, and
  overwriting it is `--force`, an explicit human decision, never the default. (The factory
  default/empty flow is deliberately treated as overwritable: `nodes=0` loses nobody's work.)
* **VERIFIED is a readback, not an assumption** — after commit the board is polled until it
  reports the compiled sha; `loadApp()` rejecting and falling back is caught here, not later.
* **Values are read from REST, never from `data.db`** — the DB is the auto-commit snapshot
  and lags the engine by up to a minute; a deploy pipeline built on it would compile stale
  thresholds. (Found by inspection while writing the frontend: the DB still held 29.6 for a
  property whose live value was 34.1.)

## Honestly reported

The scripted first pass ran the FOREIGN demo *after* stopping the CE, so `studio2riot` could
not compile and deploy exited 1 — a compile failure, not the designed refusal. `01`'s F
section is that mistake; `02` is the honest re-run with the CE up: FOREIGN detected, rc=3.
The test sequencing was wrong, not the mechanism — kept because a verdict that fails for the
wrong reason and gets shipped anyway is how confidence rots.

## What remains for the pixels

The wiresheet button/dialog renders these states (mockups: `control-engine-docs/Decisions/
riot-deploy/`). It needs an HTTP door to this pipeline — a thin endpoint the backend team can
host (the engine is theirs), or a Studio dev-server middleware. Deferred with the ghost
jump-to and the device-container ports.
