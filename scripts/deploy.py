#!/usr/bin/env python3
"""deploy — the Deploy button's decision engine, CLI-first (roadmap Phase 5).

Everything the mockups promised, minus the pixels: compile the LIVE Studio subtree, compare
fingerprints, and report one of the three states — then, only on --deploy, upload over
system.flow and verify by readback. The UI button renders this; it must never know more than
this does.

    in sync   sha(compiled) == sha(board)                 → nothing to do
    dirty     differ, board sha ∈ our deploy history      → Deploy would apply your edits
    foreign   differ, board sha ∉ history                 → someone else's flow; inspect first
              (--force overrides — an explicit human decision, never the default)

History lives beside the graph (engine/data/riot-deploy-history.json): every sha this Studio
ever deployed, newest last. That file is what makes *foreign* detectable at all — and rollback
is redeploying any earlier entry's still-stored .riot.

Usage:
  deploy.py status <uid,uid,…>
  deploy.py deploy <uid,uid,…> [--force]
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
EP = os.environ.get("RIOT_DEPLOY_EP", "tls/192.168.10.29:7447")
HIST = "/home/fw/qs/repos-no-5/ce-studio/engine/data/riot-deploy-history.json"
WORK = "/tmp/riot-deploy"


def sh(args, **kw):
    # stderr merged into stdout: a child's traceback must reach the door's log, or the widget
    # shows a bare ERROR with an empty log — the exact silence this pipeline exists to end.
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=240, **kw)


def board_status():
    r = sh([f"{APP}/build/q_zenoh_testapp", "--endpoint", EP], cwd=APP,
           input="login acbm-fabric-2026\nflow status\nquit\n")
    m = re.search(r"flow: sha=(\S+)", r.stdout)
    run = re.search(r"running=(\w+)\s+nodes=(\d+)", r.stdout)
    if not m:
        print("deploy: board unreachable or no FlowStatus")
        sys.exit(2)
    return m.group(1), (run.group(1) if run else "?"), (int(run.group(2)) if run else -1)


def compile_selection(uids):
    os.makedirs(WORK, exist_ok=True)
    fj, fr = f"{WORK}/flow.json", f"{WORK}/flow.riot"
    for cmd in ([sys.executable, f"{HERE}/studio2riot.py", uids, fj],
                [sys.executable, f"{HERE}/riotc.py", fj, fr]):
        r = sh(cmd)
        print(r.stdout.strip())
        if r.returncode != 0:
            sys.exit(1)
    blob = open(fr, "rb").read()
    return fr, hashlib.sha256(blob).hexdigest(), len(blob)


def history():
    try:
        return json.load(open(HIST))
    except (OSError, ValueError):
        return []


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("status", "deploy"):
        print(__doc__)
        return 2
    verb, uids = sys.argv[1], sys.argv[2]
    force = "--force" in sys.argv

    fr, compiled, size = compile_selection(uids)
    board, running, nodes = board_status()
    hist = history()
    known = {h["sha"] for h in hist}

    if compiled == board:
        state = "IN SYNC"
    elif board in known or nodes == 0:
        # nodes==0: the factory default / empty flow — overwriting it loses nobody's work.
        state = "DIRTY"
    else:
        state = "FOREIGN"
    print(f"deploy: compiled={compiled[:16]}… ({size} B)  board={board[:16]}… "
          f"running={running} nodes={nodes}")
    print(f"deploy: state = {state}")

    if verb == "status":
        return 0
    if state == "IN SYNC":
        print("deploy: nothing to do")
        return 0
    if state == "FOREIGN" and not force:
        print("deploy: board runs a flow this Studio has no record of — inspect first, "
              "or pass --force to overwrite (an explicit decision, never the default)")
        return 3

    r = sh([f"{APP}/build/q_zenoh_testapp", "--endpoint", EP], cwd=APP,
           input=f"login acbm-fabric-2026\nflow put {fr}\nquit\n")
    print("\n".join(l for l in r.stdout.splitlines() if "FLOW" in l or "error" in l))
    if "committed" not in r.stdout:
        print("deploy: upload did not commit — board state unchanged")
        return 1

    print("deploy: board restarting to load; verifying by readback …")
    time.sleep(8)
    deadline = time.time() + 120
    while time.time() < deadline:
        try:
            after, running, nodes = board_status()
            if after == compiled:
                break
        except SystemExit:
            pass
        time.sleep(5)
    else:
        print("deploy: readback timeout")
        return 1
    if after != compiled:
        print(f"deploy: FAILED readback — board reports {after[:16]}… "
              "(loadApp may have rejected it and fallen back)")
        return 1
    print(f"deploy: VERIFIED — board runs {after[:16]}… running={running} nodes={nodes}")
    hist.append({"sha": compiled, "size": size, "uids": uids,
                 "at": time.strftime("%Y-%m-%dT%H:%M:%S")})
    json.dump(hist, open(HIST, "w"), indent=1)
    print(f"deploy: recorded in history ({len(hist)} deploys)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
