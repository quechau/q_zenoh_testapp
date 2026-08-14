#!/usr/bin/env python3
"""studio2riot — read the LIVE Studio graph over REST and emit a riotc flow description.

The `runOn` marking is the uid list given on the command line (the Studio property lands in
Phase 5's UI pass; the compiler contract is the same either way: an explicit selection, never
an inference). Values come from REST, not from data.db — the DB is the auto-commit snapshot
and can lag the running engine by up to a minute, which is exactly the kind of quiet skew a
deploy pipeline must not build on.

Type map (refusals for anything outside it — riotc then refuses again at its own level):

  edgeLoraInput   → one lora-droplet per parent edgeLoraDevice (deduped); the input's `field`
                    picks the droplet output pin (Temperature→temp, …).
  compare         → compare-greater. Studio semantics measured on the bench: lessThan = in1<in2.
                    With the value wired to in2 and in1 a constant threshold, `lessThan` is
                    exactly Greater(value, term2:=in1).result — the mapping the comment below
                    the table encodes. in1 must NOT be wired; only `lessThan` may be consumed.
  modbusPoint     → modbus-write. Register address is 0-based in Studio and 1-based in RIOT
                    (+1 — the same off-by-one the bench docs call the easiest mistake in the
                    system). slave/port come from the parent modbusDevice.

The dual-writer refusal (measured 2026-08-13, the "write 1 đọc về 0" incident): a compiled
modbus-write point whose `in` is edge-driven on the sheet has TWO writers — the board flow
re-asserting its verdict every refresh, and the host loop re-asserting *its* verdict on every
change. When the two verdicts differ (an edited threshold not yet deployed, a board-side value
lagging), the register oscillates and the loser looks like a device fault. Until the engine
can gate host evaluation for a runOn subtree, this compiler REFUSES such selections unless
--allow-dual-writer is passed explicitly — a bench decision, never a production default.

Usage: studio2riot.py <uid,uid,…> <out.json> [--allow-dual-writer]
"""
import json
import sys
import urllib.request

API = "http://127.0.0.1:7979/api/v0"

FIELD_TO_PIN = {1: "temp", 2: "pressure", 3: "humidity", 4: "voltage"}
REG = {4: "ao", 3: "ai", 2: "do", 1: "di"}          # Studio registerType enum → riotc name
DTYPE = {0: "bool", 1: "u16", 2: "i16", 3: "u32", 4: "i32", 5: "f32"}
PORT = {1: "rs485-1", 2: "rs485-2", 3: "loraraw"}   # Studio interface enum


def get(path):
    with urllib.request.urlopen(f"{API}/{path}", timeout=5) as r:
        return json.load(r)["data"]


def node(uid):
    try:
        n = get(f"nodes/uid/{uid}")["nodes"][0]
    except Exception as e:
        # A deleted component is the single most common way a stored selection goes stale —
        # measured: the operator deleted edgeLoraInput and the widget could only say "ERROR".
        refuse(f"component {uid} no longer exists in the graph ({e}) — update the selection")
    props = {k: v.get("value") for k, v in n["properties"].items()}
    return {"uid": n["uid"], "type": n["type"].split("::")[-1], "parent": n["parent"],
            "name": n["name"], "props": props}


def refuse(msg):
    print(f"studio2riot: REFUSED: {msg}")
    sys.exit(1)


def main():
    uids = [int(u) for u in sys.argv[1].split(",")]
    comps = {u: node(u) for u in uids}
    edges = [e for e in get("edges")
             if e["sourceUid"] in comps and e["targetUid"] in comps]

    out_nodes, wires = [], []
    droplet_of = {}          # parent lora-device uid -> emitted droplet uid
    pin_of = {}              # (studio uid, studio prop) -> (riot uid, riot pin)

    for u in sorted(comps):
        c = comps[u]
        if c["type"] == "edgeLoraInput":
            dev = node(c["parent"])
            addr = str(dev["props"].get("devAddr") or "").strip()
            if len(addr) != 8:
                refuse(f"lora device {dev['uid']}: devAddr '{addr}' is not 8 hex chars")
            field = int(c["props"].get("field") or 0)
            if field not in FIELD_TO_PIN:
                refuse(f"edgeLoraInput {u}: field {field} has no droplet output pin")
            if dev["uid"] not in droplet_of:
                droplet_of[dev["uid"]] = dev["uid"]
                out_nodes.append({"uid": dev["uid"], "type": "lora-droplet",
                                  "props": {"dev_addr": addr}})
            pin_of[(u, "out")] = (droplet_of[dev["uid"]], FIELD_TO_PIN[field])
        elif c["type"] == "compare":
            if any(e["targetUid"] == u and e["targetProperty"] == "in1" for e in edges):
                refuse(f"compare {u}: in1 is wired; the mapping needs it constant (term2)")
            term2 = float(c["props"].get("in1") or 0.0)
            out_nodes.append({"uid": u, "type": "compare-greater",
                              "props": {"default_term2": term2}})
            pin_of[(u, "in2")] = (u, "value")
            pin_of[(u, "lessThan")] = (u, "result")
        elif c["type"] == "modbusPoint":
            if not c["props"].get("writable"):
                refuse(f"modbusPoint {u}: not writable — nothing for a flow to drive")
            dev = node(c["parent"])
            if dev["type"] != "modbusDevice":
                refuse(f"modbusPoint {u}: parent is {dev['type']}, not a modbusDevice")
            reg = REG.get(int(c["props"].get("registerType") or -1))
            dt = DTYPE.get(int(c["props"].get("dataType") or -1))
            port = PORT.get(int(dev["props"].get("interface") or -1))
            if not (reg and dt and port):
                refuse(f"modbusPoint {u}: unmapped registerType/dataType/interface")
            out_nodes.append({"uid": u, "type": "modbus-write", "props": {
                "port": port, "slave": int(dev["props"]["slaveId"]),
                "register": reg, "address": int(c["props"]["address"]) + 1,
                "data_type": dt, "endian": "abcd", "timeout_ms": 3000,
                "refresh_ms": 2000}})
            pin_of[(u, "in")] = (u, "value")
        else:
            refuse(f"component {u} ({c['type']}) has no board mapping")

    # The dual-writer refusal: every modbus-write we emit is also driven by the host through
    # the very edge that defines the flow. One point, one writer — see the module docstring.
    if "--allow-dual-writer" not in sys.argv:
        for u, c in comps.items():
            if c["type"] == "modbusPoint" and any(
                    e["targetUid"] == u and e["targetProperty"] == "in" for e in edges):
                refuse(f"modbusPoint {u} is written by BOTH the board flow and the host loop "
                       "(its `in` is edge-driven on the sheet). One point, one writer: either "
                       "keep the loop on the host (drop this point from the selection) or "
                       "accept the race knowingly with --allow-dual-writer. Measured: the two "
                       "writers fighting reads as a device fault (write 1, read back 0).")

    # An unwired modbus-write input would make the RIOT node write its default — NaN — to a
    # real register every refresh cycle. Refuse: a flow that would write garbage is worse than
    # no flow, and "the point is in the selection but nothing feeds it" is a graph mistake the
    # operator can actually fix.
    fed = {(e["targetUid"], e["targetProperty"]) for e in edges}
    for u, c in comps.items():
        if c["type"] == "modbusPoint" and (u, "in") not in fed:
            refuse(f"modbusPoint {u}: nothing in the selection feeds its input — the board "
                   "flow would write NaN to the register every refresh. Wire it or drop it.")

    for e in sorted(edges, key=lambda e: e["uid"]):
        s = pin_of.get((e["sourceUid"], e["sourceProperty"]))
        d = pin_of.get((e["targetUid"], e["targetProperty"]))
        if not s or not d:
            refuse(f"edge {e['uid']}: {e['sourceProperty']}→{e['targetProperty']} "
                   "uses a pin outside the mapping")
        wires.append({"src": s[0], "src_pin": s[1], "dst": d[0], "dst_pin": d[1]})

    flow = {"flow_id": 1, "nodes": out_nodes, "wires": wires}
    json.dump(flow, open(sys.argv[2], "w"), indent=1, sort_keys=True)
    print(f"studio2riot: {len(out_nodes)} nodes, {len(wires)} wires -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
