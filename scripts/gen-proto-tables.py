#!/usr/bin/env python3
"""gen-proto-tables.py — turn proto/*.proto into C lookup tables for the JSON renderer.

The renderer used to infer everything: a length-delimited field was printed as a string if
its bytes were printable, re-parsed as a nested message if that worked, and dumped as hex
otherwise. Field names came from a handful of tables typed in by hand. That is fine for one
message and does not survive 66 of them — and inference is wrong in ways that look right, so
a `bytes` field holding printable ASCII prints as a string and a small varint enum prints as
a number nobody can read.

So the schema is taken from the contract instead. This emits, for every message, its fields
with their declared numbers, names and types; for every enum, its value names; and a routing
table from (service_id, operation, direction) to the payload type carried there.

Not every payload is protobuf. `system.auth` carries a bare 32-byte SHA-256 proof and an
empty payload means logout — there is no message to look up, so those get their own payload
kinds rather than being forced through a decoder that would find nothing.

Two consumers, one script. This tool regenerates the tables at CMake configure time and keeps
nothing in source control. The ACB-M firmware commits its copy instead, next to the nanopb
output it already commits, because an ESP-IDF build should not need Python:

    python3 gen-proto-tables.py <proto-dir> \
        ACB-M/components/app_zenoh/codec/ce_schema.c \
        ACB-M/components/app_zenoh/codec/ce_schema.h rbs
"""

import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------------------
# Routing: which type is carried in the payload, per service and operation.
#
# Taken from the header comments of the contract protos themselves, e.g. modbus_points.proto:
#   READ  -> PointReadRequest in, PointValues out
#   WRITE -> PointWrite in, WriteAck out
#
# "@unknown" is used deliberately where the contract does not say. Guessing here would make
# the tool assert a shape it cannot support, which is the failure this whole change is about.
# ---------------------------------------------------------------------------------------
ROUTES = [
    # service,            op,             request payload,                  response payload
    ("system.boardinfo",  "OP_READ",      "boardinfo.GetBoardInfoRequest",  "boardinfo.GetBoardInfoResponse"),

    # system.auth has no .proto: the payload is a raw 32-byte proof and an EMPTY payload means
    # logout (ADR-016, app_zenoh.c). The reply carries its verdict in `error`, not a payload.
    ("system.auth",       "OP_EXECUTE",   "@proof",                         "@empty"),

    # The data plane is ONE schema for all three field buses (contract 0.4.5) — the key
    # says which plane a frame belongs to, the payload shape never did. The config plane
    # stays per-protocol; only its verdict is shared.
    ("modbus.points",     "OP_READ",      "common.PointReadRequest",        "common.PointValues"),
    ("modbus.points",     "OP_WRITE",     "common.PointWrite",              "common.WriteAck"),
    ("modbus.points",     "OP_SUBSCRIBE", "@unknown",                       "common.PointValues"),
    ("modbus.config",     "OP_READ",      "@empty",                         "modbus.ModbusConfig"),
    ("modbus.config",     "OP_WRITE",     "modbus.ModbusConfigDelta",       "common.ConfigResult"),

    ("bacnet.points",     "OP_READ",      "common.PointReadRequest",        "common.PointValues"),
    ("bacnet.points",     "OP_WRITE",     "common.PointWrite",              "common.WriteAck"),
    ("bacnet.points",     "OP_SUBSCRIBE", "@unknown",                       "common.PointValues"),
    ("bacnet.config",     "OP_READ",      "@empty",                         "bacnet.BacnetConfig"),
    ("bacnet.config",     "OP_WRITE",     "bacnet.BacnetConfigDelta",       "common.ConfigResult"),

    ("lora.points",       "OP_READ",      "common.PointReadRequest",        "common.PointValues"),
    ("lora.points",       "OP_WRITE",     "common.PointWrite",              "common.WriteAck"),
    ("lora.points",       "OP_SUBSCRIBE", "@unknown",                       "common.PointValues"),
    ("lora.config",       "OP_READ",      "@empty",                         "lora.LoraConfig"),
    ("lora.config",       "OP_WRITE",     "lora.LoraConfigDelta",           "common.ConfigResult"),
]

SCALARS = {
    "double": "QZ_T_DOUBLE", "float": "QZ_T_FLOAT",
    "int32": "QZ_T_VARINT", "int64": "QZ_T_VARINT",
    "uint32": "QZ_T_VARINT", "uint64": "QZ_T_VARINT",
    "sint32": "QZ_T_SVARINT", "sint64": "QZ_T_SVARINT",
    "fixed32": "QZ_T_FIXED32", "fixed64": "QZ_T_FIXED64",
    "sfixed32": "QZ_T_FIXED32", "sfixed64": "QZ_T_FIXED64",
    "bool": "QZ_T_BOOL", "string": "QZ_T_STRING", "bytes": "QZ_T_BYTES",
}

FIELD_RE = re.compile(r"^\s*(repeated\s+|optional\s+)?([\w.]+)\s+(\w+)\s*=\s*(\d+)\s*;")
EVAL_RE = re.compile(r"^\s*(\w+)\s*=\s*(-?\d+)\s*;")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(line.split("//")[0] for line in text.splitlines())


def parse(path):
    """Returns (messages, enums) as {fqname: [...]} — flat, fully qualified, package included."""
    text = strip_comments(path.read_text())
    pkg = ""
    m = re.search(r"^\s*package\s+([\w.]+)\s*;", text, flags=re.M)
    if m:
        pkg = m.group(1)

    msgs, enums = {}, {}
    stack = []          # (kind, fqname)
    for line in text.splitlines():
        s = line.strip()
        if not s:
            continue
        m = re.match(r"^(message|enum)\s+(\w+)\s*\{", s)
        if m:
            kind, name = m.group(1), m.group(2)
            parent = stack[-1][1] if stack else pkg
            fq = f"{parent}.{name}" if parent else name
            stack.append((kind, fq))
            (msgs if kind == "message" else enums)[fq] = []
            continue
        if s.startswith("}"):
            if stack:
                stack.pop()
            continue
        if not stack:
            continue
        kind, fq = stack[-1]
        if kind == "message":
            m = FIELD_RE.match(line)
            if m:
                rep = (m.group(1) or "").strip() == "repeated"
                msgs[fq].append((int(m.group(4)), m.group(3), m.group(2), rep, pkg))
        else:
            m = EVAL_RE.match(line)
            if m:
                enums[fq].append((int(m.group(2)), m.group(1)))
    return msgs, enums


def resolve(typename, pkg, msgs, enums):
    """proto name resolution, near enough: try package scope, then a unique suffix match."""
    for cand in (f"{pkg}.{typename}", typename):
        if cand in msgs:
            return "QZ_T_MSG", cand
        if cand in enums:
            return "QZ_T_ENUM", cand
    hits = [k for k in list(msgs) + list(enums) if k.endswith("." + typename)]
    if len(hits) == 1:
        return ("QZ_T_MSG" if hits[0] in msgs else "QZ_T_ENUM"), hits[0]
    return None, None


# ---------------------------------------------------------------------------------------
# TypeScript emitter — the wiresheet's Zenoh guide/packet builder (ce-studio).
#
# Same three tables as the C output, spelled the way TypeScript reads them: the C form is
# index-into-array because a firmware cannot afford anything else; the browser gets literal
# strings for types and kinds so a reader of the generated file can see what it says.
# ---------------------------------------------------------------------------------------
TS_TYPES = {
    "QZ_T_VARINT": "varint", "QZ_T_SVARINT": "svarint", "QZ_T_BOOL": "bool",
    "QZ_T_DOUBLE": "double", "QZ_T_FLOAT": "float", "QZ_T_FIXED32": "fixed32",
    "QZ_T_FIXED64": "fixed64", "QZ_T_STRING": "string", "QZ_T_BYTES": "bytes",
    "QZ_T_MSG": "msg", "QZ_T_ENUM": "enum",
}
TS_KINDS = {
    "QZ_PL_UNKNOWN": "unknown", "QZ_PL_EMPTY": "empty",
    "QZ_PL_MSG": "msg", "QZ_PL_SHA256_PROOF": "proof",
}


def _tables(msgs, enums, msg_names, enum_names, msg_idx, enum_idx, short):
    """The three tables as plain Python data — shared by the TypeScript and JS emitters."""
    out_msgs = []
    for name in msg_names:
        fields = []
        for num, fname, ftype, rep, pkg in msgs[name]:
            if ftype in SCALARS:
                t, ref = SCALARS[ftype], -1
            else:
                kind, fq = resolve(ftype, pkg, msgs, enums)
                if kind is None:
                    t, ref = "QZ_T_BYTES", -1
                else:
                    t = kind
                    ref = (msg_idx if kind == "QZ_T_MSG" else enum_idx)[fq]
            fields.append({"number": num, "name": fname, "type": TS_TYPES[t],
                           "ref": ref, "repeated": bool(rep)})
        out_msgs.append({"name": name, "fields": fields})

    out_enums = [{"name": n, "values": [{"value": v, "name": vn} for v, vn in enums[n]]}
                 for n in enum_names]

    op_enum = next((k for k in enum_names if k.endswith("ServiceOperation")), None)
    op_num = {v: n for n, v in enums[op_enum]} if op_enum else {}

    def slot(spec):
        if spec == "@proof":
            return -1, "proof"
        if spec == "@empty":
            return -1, "empty"
        if spec == "@unknown":
            return -1, "unknown"
        fq = short(spec)
        if fq is None:
            sys.exit(f"unresolved route type: {spec}")
        return msg_idx[fq], "msg"

    out_routes = []
    for service, op, req, rsp in ROUTES:
        ri, rk = slot(req)
        si, sk = slot(rsp)
        out_routes.append({"service": service, "op": op_num.get(op, 0), "req": ri, "rsp": si,
                           "reqKind": rk, "rspKind": sk})
    return out_msgs, out_enums, out_routes


def emit_js(out_js, msgs, enums, msg_names, enum_names, msg_idx, enum_idx, short, nfiles):
    """A CLASSIC script (not a module) setting window.CE_CONTRACT.

    The consumer is Contracts/ce-edgelink-acb-m/detail.html, a learning page people open
    straight off disk with file://. ES module imports are blocked there by CORS; a classic
    <script src> next to the page is not — so this is deliberately not a module.
    """
    m, e, r = _tables(msgs, enums, msg_names, enum_names, msg_idx, enum_idx, short)
    import json as _json
    body = _json.dumps({"messages": m, "enums": e, "routes": r}, ensure_ascii=False,
                       separators=(",", ":"))
    out_js.write_text(
        "/* GENERATED by q_zenoh_testapp/scripts/gen-proto-tables.py --js — do not edit.\n"
        " *\n"
        " * The contract as data for the packet builder in detail.html: every message's fields,\n"
        " * every enum's values, and which payload each (service, op, direction) carries. Same\n"
        " * script emits ce_schema.c (host + firmware) and ceContract.generated.ts (wiresheet),\n"
        " * so a .proto change moves all four together.\n"
        " *\n"
        " * Classic script on purpose: the page is opened over file://, where module imports are\n"
        " * blocked but <script src> is not.\n"
        " */\n"
        "window.CE_CONTRACT = " + body + ";\n")
    print(f"gen-proto-tables --js: {len(m)} messages, {len(e)} enums, {len(r)} routes "
          f"from {nfiles} files -> {out_js}")


def emit_ts(out_ts, msgs, enums, msg_names, enum_names, msg_idx, enum_idx, short, nfiles):
    o = ["""// GENERATED by q_zenoh_testapp/scripts/gen-proto-tables.py --ts — do not edit.
//
// The contract as data: every message's fields (declared number, name, type), every enum's
// values, and which payload message each (service, op, direction) carries. Regenerate with
//
//     python3 q_zenoh_testapp/scripts/gen-proto-tables.py q_zenoh_testapp/proto \\
//         --ts ce-studio/wiresheet/src/lib/ceContract.generated.ts
//
// The same script emits ce-edgelink's ce_schema.c and ACB-M's copy, so a builder driven by
// this table encodes exactly what the host encoder and the board decoder agree on.

export type CeFieldType =
  | "varint" | "svarint" | "bool" | "double" | "float"
  | "fixed32" | "fixed64" | "string" | "bytes" | "msg" | "enum";

export interface CeField {
  number: number;
  name: string;
  type: CeFieldType;
  /** Index into CE_MESSAGES (type "msg") or CE_ENUMS (type "enum"); -1 for scalars. */
  ref: number;
  repeated: boolean;
}

export interface CeMessage {
  name: string;
  fields: CeField[];
}

export interface CeEnumValue {
  value: number;
  name: string;
}

export interface CeEnum {
  name: string;
  values: CeEnumValue[];
}

/** What a payload holds. Not every payload is protobuf: system.auth carries a bare 32-byte
 *  SHA-256 proof ("proof"), and an empty payload there means logout. */
export type CePayloadKind = "unknown" | "empty" | "msg" | "proof";

export interface CeRoute {
  service: string;
  op: number;
  /** Index into CE_MESSAGES, or -1 when the kind is not "msg". */
  req: number;
  rsp: number;
  reqKind: CePayloadKind;
  rspKind: CePayloadKind;
}

export const CE_MESSAGES: readonly CeMessage[] = ["""]

    for name in msg_names:
        fields = msgs[name]
        if not fields:
            o.append(f'  {{ name: "{name}", fields: [] }},')
            continue
        o.append(f'  {{\n    name: "{name}",\n    fields: [')
        for num, fname, ftype, rep, pkg in fields:
            if ftype in SCALARS:
                t, ref = SCALARS[ftype], -1
            else:
                kind, fq = resolve(ftype, pkg, msgs, enums)
                if kind is None:
                    t, ref = "QZ_T_BYTES", -1
                else:
                    t = kind
                    ref = (msg_idx if kind == "QZ_T_MSG" else enum_idx)[fq]
            o.append(f'      {{ number: {num}, name: "{fname}", type: "{TS_TYPES[t]}", '
                     f'ref: {ref}, repeated: {"true" if rep else "false"} }},')
        o.append("    ],\n  },")
    o.append("];\n")

    o.append("export const CE_ENUMS: readonly CeEnum[] = [")
    for name in enum_names:
        o.append(f'  {{\n    name: "{name}",\n    values: [')
        for val, vname in enums[name]:
            o.append(f'      {{ value: {val}, name: "{vname}" }},')
        o.append("    ],\n  },")
    o.append("];\n")

    op_enum = next((k for k in enum_names if k.endswith("ServiceOperation")), None)
    op_num = {v: n for n, v in enums[op_enum]} if op_enum else {}

    def slot(spec):
        if spec == "@proof":
            return -1, "QZ_PL_SHA256_PROOF"
        if spec == "@empty":
            return -1, "QZ_PL_EMPTY"
        if spec == "@unknown":
            return -1, "QZ_PL_UNKNOWN"
        fq = short(spec)
        if fq is None:
            sys.exit(f"unresolved route type: {spec}")
        return msg_idx[fq], "QZ_PL_MSG"

    o.append("export const CE_ROUTES: readonly CeRoute[] = [")
    for service, op, req, rsp in ROUTES:
        ri, rk = slot(req)
        si, sk = slot(rsp)
        o.append(f'  {{ service: "{service}", op: {op_num.get(op, 0)}, req: {ri}, rsp: {si}, '
                 f'reqKind: "{TS_KINDS[rk]}", rspKind: "{TS_KINDS[sk]}" }},')
    o.append("];\n")

    out_ts.write_text("\n".join(o))
    print(f"gen-proto-tables --ts: {len(msg_names)} messages, {len(enum_names)} enums, "
          f"{len(ROUTES)} routes from {nfiles} files -> {out_ts}")


def main():
    proto_dir = Path(sys.argv[1])
    # `--ts <file>` emits the SAME tables as TypeScript instead of C. Third consumer, same
    # script: the wiresheet's packet builder needs the field list of every message to offer
    # typed inputs, and a hand-kept copy in the browser would drift from the board the first
    # time a .proto changes — silently, because the wire still encodes.
    ts_out = js_out = None
    out_c = out_h = None
    prefix = "qz"
    if len(sys.argv) > 2 and sys.argv[2] == "--ts":
        ts_out = Path(sys.argv[3])
    elif len(sys.argv) > 2 and sys.argv[2] == "--js":
        js_out = Path(sys.argv[3])
    else:
        out_c, out_h = Path(sys.argv[2]), Path(sys.argv[3])
        # The same tables are compiled into the firmware, which has its own naming conventions
        # and its own copy of nothing else from this repo. A prefix keeps the two from reading
        # like foreign code in each other's tree.
        prefix = sys.argv[4] if len(sys.argv) > 4 else "qz"
    # common/ carries the definitions every service shares (contract 0.4.5): one data
    # plane and one config verdict instead of three field-for-field copies. It must be
    # parsed too, or every route that points at a shared message resolves to nothing.
    files = (sorted(proto_dir.glob("*.proto")) + sorted(proto_dir.glob("common/*.proto"))
             + sorted(proto_dir.glob("services/*.proto")))
    if not files:
        sys.exit(f"no .proto files under {proto_dir}")

    msgs, enums = {}, {}
    for f in files:
        m, e = parse(f)
        msgs.update(m)
        enums.update(e)

    msg_names = sorted(msgs)
    enum_names = sorted(enums)
    msg_idx = {n: i for i, n in enumerate(msg_names)}
    enum_idx = {n: i for i, n in enumerate(enum_names)}

    def short(fq):
        """Route names are written `modbus.PointValues`; match them against the full name."""
        for cand in msg_names:
            if cand == fq or cand.endswith("." + fq.split(".", 1)[-1]) and fq.split(".")[0] in cand:
                return cand
        return None

    if ts_out is not None:
        emit_ts(ts_out, msgs, enums, msg_names, enum_names, msg_idx, enum_idx, short, len(files))
        return

    if js_out is not None:
        emit_js(js_out, msgs, enums, msg_names, enum_names, msg_idx, enum_idx, short, len(files))
        return

    out = []
    out.append('/* GENERATED by scripts/gen-proto-tables.py — do not edit. */\n')
    out.append(f'#include "{out_h.name}"\n#include <string.h>\n\n')

    for name in msg_names:
        fields = msgs[name]
        if not fields:
            # Zero-field message (e.g. FlowCommit): the msgs table row carries NULL/0, so
            # no field array is emitted at all — a tentative array would trip
            # -Werror=unused-const-variable on the firmware build.
            pass
            continue
        out.append(f"/* {name} */\nstatic const qz_pfield_t f_{msg_idx[name]}[] = {{\n")
        for num, fname, ftype, rep, pkg in fields:
            if ftype in SCALARS:
                t, ref = SCALARS[ftype], -1
            else:
                kind, fq = resolve(ftype, pkg, msgs, enums)
                if kind is None:
                    t, ref = "QZ_T_BYTES", -1
                else:
                    t = kind
                    ref = (msg_idx if kind == "QZ_T_MSG" else enum_idx)[fq]
            out.append(f'    {{ {num}, "{fname}", {t}, {ref}, {1 if rep else 0} }},\n')
        out.append("};\n")

    out.append("\nconst qz_pmsg_t qz_msgs[] = {\n")
    for name in msg_names:
        n = len(msgs[name])
        arr = f"f_{msg_idx[name]}" if n else "NULL"
        out.append(f'    {{ "{name}", {arr}, {n} }},\n')
    out.append("};\nconst int qz_nmsgs = (int)(sizeof qz_msgs / sizeof qz_msgs[0]);\n\n")

    for name in enum_names:
        out.append(f"/* {name} */\nstatic const qz_pev_t e_{enum_idx[name]}[] = {{\n")
        for val, vname in enums[name]:
            out.append(f'    {{ {val}, "{vname}" }},\n')
        out.append("};\n")

    out.append("\nconst qz_penum_t qz_enums[] = {\n")
    for name in enum_names:
        out.append(f'    {{ "{name}", e_{enum_idx[name]}, {len(enums[name])} }},\n')
    out.append("};\nconst int qz_nenums = (int)(sizeof qz_enums / sizeof qz_enums[0]);\n\n")

    # Routing table -------------------------------------------------------------------
    op_enum = next((k for k in enum_names if k.endswith("ServiceOperation")), None)
    op_num = {v: n for n, v in enums[op_enum]} if op_enum else {}

    out.append("static const struct { const char *service; int op; int req; int rsp;\n"
               "                     qz_payload_kind_t reqk, rspk; } qz_routes[] = {\n")
    unresolved = []
    for service, op, req, rsp in ROUTES:
        def slot(spec):
            if spec == "@proof":
                return -1, "QZ_PL_SHA256_PROOF"
            if spec == "@empty":
                return -1, "QZ_PL_EMPTY"
            if spec == "@unknown":
                return -1, "QZ_PL_UNKNOWN"
            fq = short(spec)
            if fq is None:
                unresolved.append(f"{service}/{op}: {spec}")
                return -1, "QZ_PL_UNKNOWN"
            return msg_idx[fq], "QZ_PL_MSG"
        ri, rk = slot(req)
        si, sk = slot(rsp)
        out.append(f'    {{ "{service}", {op_num.get(op, 0)}, {ri}, {si}, {rk}, {sk} }},\n')
    out.append("};\n")
    if unresolved:
        sys.exit("unresolved route types:\n  " + "\n  ".join(unresolved))

    out.append('''
const qz_pmsg_t *qz_msg_find(const char *name)
{
    for (int i = 0; i < qz_nmsgs; i++) {
        const char *n = qz_msgs[i].name;
        if (strcmp(n, name) == 0) return &qz_msgs[i];
        size_t ln = strlen(n), lq = strlen(name);
        if (ln > lq && n[ln - lq - 1] == '.' && strcmp(n + ln - lq, name) == 0) return &qz_msgs[i];
    }
    return NULL;
}

const char *qz_enum_label(int ref, uint64_t value)
{
    if (ref < 0 || ref >= qz_nenums) return NULL;
    for (int i = 0; i < qz_enums[ref].nvals; i++)
        if (qz_enums[ref].vals[i].value == value) return qz_enums[ref].vals[i].name;
    return NULL;
}

qz_payload_kind_t qz_payload_type(const char *service, uint64_t op, bool response,
                                  const qz_pmsg_t **out)
{
    if (out != NULL) *out = NULL;
    if (service == NULL) return QZ_PL_UNKNOWN;
    for (size_t i = 0; i < sizeof qz_routes / sizeof qz_routes[0]; i++) {
        if (strcmp(qz_routes[i].service, service) != 0) continue;
        if ((uint64_t)qz_routes[i].op != op) continue;
        int idx = response ? qz_routes[i].rsp : qz_routes[i].req;
        qz_payload_kind_t k = response ? qz_routes[i].rspk : qz_routes[i].reqk;
        if (k == QZ_PL_MSG && idx >= 0 && out != NULL) *out = &qz_msgs[idx];
        return k;
    }
    return QZ_PL_UNKNOWN;
}
''')
    def rename(text):
        return (text.replace("qz_", f"{prefix}_")
                    .replace("QZ_", f"{prefix.upper()}_"))

    out_c.write_text(rename("".join(out)))

    out_h.write_text(rename('''/* GENERATED by scripts/gen-proto-tables.py — do not edit.
 *
 * The contract schema as C tables: every message's fields with their declared numbers, names
 * and types; every enum's value names; and which payload type each (service, op, direction)
 * carries. The JSON renderer reads declared types from here instead of inferring them.
 */
#ifndef QZ_PROTO_TABLES_H
#define QZ_PROTO_TABLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    QZ_T_VARINT, QZ_T_SVARINT, QZ_T_BOOL, QZ_T_DOUBLE, QZ_T_FLOAT,
    QZ_T_FIXED32, QZ_T_FIXED64, QZ_T_STRING, QZ_T_BYTES, QZ_T_MSG, QZ_T_ENUM
} qz_type_t;

typedef struct {
    uint32_t   number;
    const char *name;
    qz_type_t  type;
    int        ref;        /* index into qz_msgs or qz_enums, by type; -1 for scalars */
    int        repeated;
} qz_pfield_t;

typedef struct { const char *name; const qz_pfield_t *fields; int nfields; } qz_pmsg_t;
typedef struct { uint64_t value; const char *name; } qz_pev_t;
typedef struct { const char *name; const qz_pev_t *vals; int nvals; } qz_penum_t;

extern const qz_pmsg_t  qz_msgs[];
extern const int        qz_nmsgs;
extern const qz_penum_t qz_enums[];
extern const int        qz_nenums;

/** What a payload holds. Not every payload is protobuf, so the decoder is told rather than
 *  left to discover: `system.auth` carries a bare SHA-256 proof, and an empty one is a
 *  logout. QZ_PL_UNKNOWN means the contract does not say — the renderer then falls back to
 *  inference and labels fields by number. */
typedef enum { QZ_PL_UNKNOWN = 0, QZ_PL_EMPTY, QZ_PL_MSG, QZ_PL_SHA256_PROOF } qz_payload_kind_t;

const qz_pmsg_t  *qz_msg_find(const char *name);
const char       *qz_enum_label(int ref, uint64_t value);
qz_payload_kind_t qz_payload_type(const char *service, uint64_t op, bool response,
                                  const qz_pmsg_t **out);

#ifdef __cplusplus
}
#endif

#endif /* QZ_PROTO_TABLES_H */
'''))
    print(f"gen-proto-tables: {len(msg_names)} messages, {len(enum_names)} enums, "
          f"{len(ROUTES)} routes from {len(files)} files")


if __name__ == "__main__":
    main()
