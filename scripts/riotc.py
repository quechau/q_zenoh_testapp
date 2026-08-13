#!/usr/bin/env python3
"""riotc — compile a flow description into a .riot application (roadmap Phase 4).

Input is a JSON flow: nodes with a type, properties and an integer uid, and wires between
node pins. Output is the binary `loadApp()` expects, per `riot-engine/docs/riot_file.md`.
The Studio integration will feed this from its SQLite graph; the compiler core takes JSON so
it can be tested — and blamed — separately from any UI.

Two rules carry the design:

DETERMINISM. The Deploy button's whole truth is sha(compiled) == sha(deployed). Everything
here is emitted in uid order — packages by the lowest uid using them, nodes and instances by
uid — so recompiling an unchanged graph is byte-identical. An iteration-order compiler would
give the same graph a new hash on every run and the dirty flag would lie forever (the ADR-024
slot-order lesson, promoted to a requirement).

REFUSAL. Anything this compiler is not sure of is a named error, never a guess and never a
silent trim: unknown node type, missing/invalid property, a wire to a pin that does not
exist, an instance budget overflow, a file over the NVS bound. Measured on the bench
(2026-08-13): `loadApp()` erases anything it dislikes and falls back to the default flow, so
a guessed byte does not merely fail — it silently un-deploys.

Node types (the packages ACB-M registers; extend the table, never special-case):

  lora-droplet     props: dev_addr (8 hex chars)          outputs: temp,pressure,humidity,…
  compare-greater  props: default_term2 (float)           in: value,term2   out: result,result_eq
  compare-less     props: default_term2 (float)           in: value,term2   out: result,result_eq
  modbus-write     props: port rs485-1|rs485-2|loraraw, slave 1..247, register di|do|ai|ao,
                          address (1-based), data_type bool|u16|i16|u32|i32|f32,
                          endian abcd|badc|cdab|dcba, timeout_ms, refresh_ms
                                                         in: value         out: value

Usage: riotc.py <flow.json> <out.riot>   (prints the sha256; exits non-zero on refusal)
"""
import hashlib
import json
import struct
import sys

MAGIC = 0xAA55CC33
MAX_FILE = 8192          # PARAM_RIOT_FLOW_APP bound
MIN_ENGINE = b"\x01\x00\x00"   # loadApp() is strict about this — measured, not assumed


class Refusal(Exception):
    pass


def dp_custom(payload: bytes) -> bytes:
    """One custom data point (id 15). Length rides in the specific-info nibble."""
    if not 0 < len(payload) <= 15:
        raise Refusal(f"custom data point of {len(payload)} bytes needs the long form "
                      "(specific-info 0), which nothing here should require")
    return bytes([0xF0 | len(payload)]) + payload


def f32(v: float) -> bytes:
    return struct.pack("<f", float(v))


def u16(v: int) -> bytes:
    return struct.pack("<H", int(v))


# ---- the type table: package id, node id, pin names, and the datapoint emitter ------------

MB_ENUM = {
    "port": {"loraraw": 0, "rs485-1": 1, "rs485-2": 2},
    "register": {"di": 0, "do": 1, "ai": 2, "ao": 3},
    "data_type": {"bool": 0, "u16": 1, "i16": 2, "u32": 3, "i32": 4, "f32": 5},
    "endian": {"abcd": 0, "badc": 1, "cdab": 2, "dcba": 3},
}


def _enum(node, key):
    v = str(prop(node, key)).lower()
    if v not in MB_ENUM[key]:
        raise Refusal(f"node {node['uid']} ({node['type']}): {key} '{v}' is not one of "
                      f"{sorted(MB_ENUM[key])}")
    return MB_ENUM[key][v]


def prop(node, key):
    try:
        return node["props"][key]
    except KeyError:
        raise Refusal(f"node {node['uid']} ({node['type']}): missing property '{key}'")


def dps_droplet(node):
    addr = str(prop(node, "dev_addr")).upper()
    if len(addr) != 8 or any(c not in "0123456789ABCDEF" for c in addr):
        raise Refusal(f"node {node['uid']}: dev_addr must be 8 hex chars, got '{addr}'")
    return [dp_custom(addr.encode())]


def dps_compare(node):
    return [dp_custom(f32(prop(node, "default_term2")))]


def dps_modbus_write(node):
    slave = int(prop(node, "slave"))
    if not 1 <= slave <= 247:
        raise Refusal(f"node {node['uid']}: slave {slave} outside 1..247")
    address = int(prop(node, "address"))
    if not 1 <= address <= 65535:
        raise Refusal(f"node {node['uid']}: address {address} outside 1..65535 (1-based)")
    return [
        dp_custom(bytes([_enum(node, "port")])),
        dp_custom(bytes([slave])),
        dp_custom(bytes([_enum(node, "register")])),
        dp_custom(u16(address)),
        dp_custom(bytes([_enum(node, "data_type")])),
        dp_custom(bytes([_enum(node, "endian")])),
        dp_custom(u16(int(prop(node, "timeout_ms")))),
        dp_custom(u16(int(prop(node, "refresh_ms")))),
    ]


TYPES = {
    #                pkg     node  inputs                outputs                                  dps
    "lora-droplet":   (0x4F4C, 1, [],                   ["temp", "pressure", "humidity",
                                                         "voltage", "co2", "movement"],          dps_droplet),
    "compare-greater": (0x6F63, 2, ["value", "term2"],  ["result", "result_eq"],                 dps_compare),
    "compare-less":   (0x6F63, 3, ["value", "term2"],   ["result", "result_eq"],                 dps_compare),
    "modbus-write":   (0x626D, 2, ["value"],            ["value"],                               dps_modbus_write),
}


def compile_flow(flow: dict) -> bytes:
    nodes = flow.get("nodes", [])
    wires = flow.get("wires", [])
    if not nodes:
        raise Refusal("flow has no nodes")

    for n in nodes:
        if n.get("type") not in TYPES:
            raise Refusal(f"node {n.get('uid')}: type '{n.get('type')}' has no RIOT package "
                          f"(known: {sorted(TYPES)})")
    uids = [n["uid"] for n in nodes]
    if len(set(uids)) != len(uids):
        raise Refusal("duplicate node uids")

    # Instance ids are assigned by uid order — the determinism rule. 12 bits on the wire.
    order = sorted(nodes, key=lambda n: n["uid"])
    inst = {n["uid"]: i + 1 for i, n in enumerate(order)}
    if len(order) > 0xFFF:
        raise Refusal("more than 4095 instances")

    # Wires resolved to (dst uid) -> list of (src inst, src out idx, dst in idx), dst-in unique.
    by_dst = {n["uid"]: [] for n in nodes}
    node_of = {n["uid"]: n for n in nodes}
    seen_dst_pin = set()
    for w in sorted(wires, key=lambda w: (w["dst"], w["dst_pin"], w["src"], w["src_pin"])):
        for end, pin, io in (("src", "src_pin", 3), ("dst", "dst_pin", 2)):
            if w[end] not in node_of:
                raise Refusal(f"wire references unknown node uid {w[end]}")
            names = TYPES[node_of[w[end]]["type"]][io]
            if w[pin] not in names:
                raise Refusal(f"wire {end} pin '{w[pin]}' does not exist on "
                              f"{node_of[w[end]]['type']} (has: {names})")
        if (w["dst"], w["dst_pin"]) in seen_dst_pin:
            raise Refusal(f"input {w['dst']}.{w['dst_pin']} is driven by two wires")
        seen_dst_pin.add((w["dst"], w["dst_pin"]))
        src_t, dst_t = TYPES[node_of[w["src"]]["type"]], TYPES[node_of[w["dst"]]["type"]]
        by_dst[w["dst"]].append((inst[w["src"]], src_t[3].index(w["src_pin"]),
                                 dst_t[2].index(w["dst_pin"])))

    # Group: package (by lowest using uid) -> node id (by lowest using uid) -> instances by uid.
    pkg_first, node_first = {}, {}
    for n in order:
        pkg, nid = TYPES[n["type"]][0], TYPES[n["type"]][1]
        pkg_first.setdefault(pkg, n["uid"])
        node_first.setdefault((pkg, nid), n["uid"])

    payload = bytearray(u16(flow.get("flow_id", 0xFFFF)))
    pkgs = sorted(pkg_first, key=lambda p: pkg_first[p])
    payload += bytes([len(pkgs)])
    for pkg in pkgs:
        nids = sorted({TYPES[n["type"]][1] for n in order if TYPES[n["type"]][0] == pkg},
                      key=lambda nid: node_first[(pkg, nid)])
        payload += u16(pkg) + bytes([len(nids)])
        for nid in nids:
            members = [n for n in order
                       if TYPES[n["type"]][0] == pkg and TYPES[n["type"]][1] == nid]
            payload += bytes([nid, len(members)])
            for n in members:
                conns = sorted(by_dst[n["uid"]], key=lambda c: c[2])
                if len(conns) > 0xF:
                    raise Refusal(f"node {n['uid']}: {len(conns)} input connections > 15")
                payload += u16((inst[n["uid"]] << 4) | len(conns))
                for src_inst, out_idx, in_idx in conns:
                    payload += u16(src_inst << 4) + bytes([(out_idx << 4) | in_idx])
                dps = TYPES[n["type"]][4](n)
                payload += bytes([len(dps)]) + b"".join(dps)

    total = 12 + len(payload)
    if total > MAX_FILE:
        raise Refusal(f"compiled flow is {total} B; the NVS bound is {MAX_FILE} B")
    f = bytearray(struct.pack("<IBBH", MAGIC, 1, 1, total)) + b"\x00" + MIN_ENGINE + payload
    f[8] = (-sum(f)) & 0xFF          # LRC: the whole file must sum to zero
    return bytes(f)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    flow = json.load(open(sys.argv[1]))
    try:
        out = compile_flow(flow)
    except Refusal as e:
        print(f"riotc: REFUSED: {e}")
        return 1
    open(sys.argv[2], "wb").write(out)
    print(f"riotc: {sys.argv[2]}: {len(out)} bytes sha256={hashlib.sha256(out).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
