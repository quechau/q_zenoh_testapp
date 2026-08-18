/** inventory.c — one view of everything a board is holding, grouped the way it is owned.
 *
 * `config <proto> read` answers one plane at a time and prints the raw reply. That is the right
 * tool when you are debugging the wire, and the wrong one when the question is operational:
 * which points exist, which device each belongs to, and which of them you may write.
 *
 * The board is the authority on this, not the host: it is the only thing that knows what it
 * actually applied, as opposed to what someone tried to provision. Since ADR-024 the board also
 * keeps that config across a reboot, so this answers a second question it could not answer
 * before — whether a board came back holding what it held, or came back empty. Each plane's
 * line carries the board's config fingerprint for exactly that comparison.
 *
 * Nothing here knows a Modbus register from a BACnet object. The three config planes are walked
 * with the generated contract schema, so a field added to the contract appears here without a
 * line changing.
 */
#include "qz.h"
#include "proto_tables.h"

#include <stdio.h>
#include <string.h>

/* A config snapshot is devices in field 1 and points in field 2, for all three protocols —
 * ModbusConfig, BacnetConfig and LoraConfig agree on that much. */
#define FIELD_DEVICES 1
#define FIELD_POINTS  2

typedef struct {
    const char *proto;          /* "modbus" / "bacnet" / "lora" */
    const char *config_msg;     /* the snapshot itself, for its config_hash */
    const char *device_msg;     /* fully qualified, because PointDef exists in three packages */
    const char *point_msg;
} plane_t;

static const plane_t PLANES[] = {
    { "modbus", "ce.embedded.modbus.v1.ModbusConfig",
      "ce.embedded.modbus.v1.ModbusDeviceDef", "ce.embedded.modbus.v1.ModbusPointDef" },
    { "bacnet", "ce.embedded.bacnet.v1.BacnetConfig",
      "ce.embedded.bacnet.v1.BacnetDeviceDef", "ce.embedded.bacnet.v1.BacnetPointDef" },
    { "lora",   "ce.embedded.lora.v1.LoraConfig",
      "ce.embedded.lora.v1.LoraDeviceDef",     "ce.embedded.lora.v1.LoraPointDef" },
};

static bool read_varint(const uint8_t *b, size_t len, size_t *i, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (*i < len) {
        uint8_t c = b[(*i)++];
        v |= (uint64_t)(c & 0x7F) << shift;
        if ((c & 0x80) == 0) { *out = v; return true; }
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

/** Value of one field of a message, rendered the way its declared type reads. */
static void field_str(const qz_pmsg_t *msg, const uint8_t *b, size_t len, const char *want,
                      char *out, size_t cap)
{
    out[0] = '\0';
    size_t i = 0;
    while (i < len) {
        uint64_t key;
        if (!read_varint(b, len, &i, &key)) return;
        uint32_t number = (uint32_t)(key >> 3), wt = (uint32_t)(key & 7);

        const qz_pfield_t *f = NULL;
        for (int k = 0; msg != NULL && k < msg->nfields; k++)
            if (msg->fields[k].number == number) { f = &msg->fields[k]; break; }

        if (wt == 0) {
            uint64_t v;
            if (!read_varint(b, len, &i, &v)) return;
            if (f != NULL && strcmp(f->name, want) == 0) {
                const char *label = (f->type == QZ_T_ENUM) ? qz_enum_label(f->ref, v) : NULL;
                if (label != NULL)               snprintf(out, cap, "%s", label);
                else if (f->type == QZ_T_BOOL)   snprintf(out, cap, "%s", v ? "yes" : "no");
                else                             snprintf(out, cap, "%llu", (unsigned long long)v);
                return;
            }
        } else if (wt == 2) {
            uint64_t l;
            if (!read_varint(b, len, &i, &l) || i + l > len) return;
            if (f != NULL && strcmp(f->name, want) == 0) {
                size_t n = (size_t)l < cap - 1 ? (size_t)l : cap - 1;
                memcpy(out, b + i, n);
                out[n] = '\0';
                return;
            }
            i += l;
        } else if (wt == 5) {
            if (i + 4 > len) return;
            if (f != NULL && strcmp(f->name, want) == 0) {
                float g;
                memcpy(&g, b + i, 4);
                snprintf(out, cap, "%g", (double)g);
                return;
            }
            i += 4;
        } else if (wt == 1) {
            if (i + 8 > len) return;
            if (f != NULL && strcmp(f->name, want) == 0) {
                double d;
                memcpy(&d, b + i, 8);
                snprintf(out, cap, "%g", d);
                return;
            }
            i += 8;
        } else return;
    }
}

/** Calls `fn` for every occurrence of `field` in a config snapshot. */
static void each_entry(const uint8_t *b, size_t len, uint32_t field,
                       void (*fn)(const uint8_t *, size_t, void *), void *arg)
{
    size_t i = 0;
    while (i < len) {
        uint64_t key;
        if (!read_varint(b, len, &i, &key)) return;
        uint32_t number = (uint32_t)(key >> 3), wt = (uint32_t)(key & 7);
        if (wt == 2) {
            uint64_t l;
            if (!read_varint(b, len, &i, &l) || i + l > len) return;
            if (number == field) fn(b + i, (size_t)l, arg);
            i += l;
        } else if (wt == 0) {
            uint64_t v;
            if (!read_varint(b, len, &i, &v)) return;
        } else if (wt == 5) i += 4;
        else if (wt == 1)   i += 8;
        else return;
    }
}

typedef struct {
    const plane_t   *plane;
    const qz_pmsg_t *point_msg;
    const char      *device_id;     /* the device whose points we are printing */
    unsigned         printed;
} point_ctx_t;

static void print_point(const uint8_t *b, size_t len, void *arg)
{
    point_ctx_t *pc = (point_ctx_t *)arg;
    char ref[32], id[32], name[64], writable[16];
    field_str(pc->point_msg, b, len, "device_ref", ref, sizeof ref);
    if (strcmp(ref, pc->device_id) != 0) return;      /* belongs to another device */

    field_str(pc->point_msg, b, len, "point_id", id, sizeof id);
    field_str(pc->point_msg, b, len, "name", name, sizeof name);
    field_str(pc->point_msg, b, len, "writable", writable, sizeof writable);

    /* What identifies a point on the wire differs per protocol, and that difference is the
     * whole reason the config planes are not interchangeable. Show each in its own terms. */
    char where[64] = "";
    if (strcmp(pc->plane->proto, "modbus") == 0) {
        char reg[24], addr[16], dtype[16];
        field_str(pc->point_msg, b, len, "reg_type", reg, sizeof reg);
        field_str(pc->point_msg, b, len, "address", addr, sizeof addr);
        field_str(pc->point_msg, b, len, "data_type", dtype, sizeof dtype);
        snprintf(where, sizeof where, "%s:%s %s", reg[0] ? reg + 4 : "?",   /* drop REG_ */
                 addr[0] ? addr : "0", dtype[0] ? dtype + 3 : "");          /* drop DT_  */
    } else if (strcmp(pc->plane->proto, "bacnet") == 0) {
        char obj[24], inst[16];
        field_str(pc->point_msg, b, len, "obj_type", obj, sizeof obj);
        field_str(pc->point_msg, b, len, "obj_instance", inst, sizeof inst);
        snprintf(where, sizeof where, "%s:%s", obj[0] ? obj + 4 : "?",      /* drop OBJ_ */
                 inst[0] ? inst : "0");
    } else {
        char fld[32];
        field_str(pc->point_msg, b, len, "field", fld, sizeof fld);
        snprintf(where, sizeof where, "%s", fld[0] ? fld + 6 : "?");        /* drop FIELD_ */
    }

    printf("      point %-8s %-18s %-4s %s\n", id, where,
           strcmp(writable, "yes") == 0 ? "w" : "ro", name);
    pc->printed++;
}

typedef struct {
    const plane_t   *plane;
    const qz_pmsg_t *device_msg;
    const qz_pmsg_t *point_msg;
    const uint8_t   *snapshot;
    size_t           snapshot_len;
    unsigned         devices;
} device_ctx_t;

static void print_device(const uint8_t *b, size_t len, void *arg)
{
    device_ctx_t *dc = (device_ctx_t *)arg;
    char id[32], enabled[16], line[96] = "";
    field_str(dc->device_msg, b, len, "device_id", id, sizeof id);
    field_str(dc->device_msg, b, len, "enabled", enabled, sizeof enabled);

    if (strcmp(dc->plane->proto, "modbus") == 0) {
        char unit[16], iface[24], baud[16], parity[24];
        field_str(dc->device_msg, b, len, "unit_id", unit, sizeof unit);
        field_str(dc->device_msg, b, len, "interface", iface, sizeof iface);
        field_str(dc->device_msg, b, len, "baud", baud, sizeof baud);
        field_str(dc->device_msg, b, len, "parity", parity, sizeof parity);
        snprintf(line, sizeof line, "unit %-4s %-10s %s %s", unit,
                 iface[0] ? iface + 3 : "",                                  /* drop IF_ */
                 baud[0] && strcmp(baud, "0") != 0 ? baud : "port-default",
                 parity[0] ? parity + 4 : "");                               /* drop PAR_ */
    } else if (strcmp(dc->plane->proto, "bacnet") == 0) {
        char mac[16];
        field_str(dc->device_msg, b, len, "mac_addr", mac, sizeof mac);
        snprintf(line, sizeof line, "MS/TP mac %s", mac);
    } else {
        char addr[24];
        field_str(dc->device_msg, b, len, "dev_addr", addr, sizeof addr);
        snprintf(line, sizeof line, "dev_addr %s", addr);
    }

    printf("    device %-6s %-38s %s\n", id, line,
           strcmp(enabled, "yes") == 0 ? "enabled" : "DISABLED");

    point_ctx_t pc = { dc->plane, dc->point_msg, id, 0 };
    each_entry(dc->snapshot, dc->snapshot_len, FIELD_POINTS, print_point, &pc);
    if (pc.printed == 0) printf("      (no points)\n");
    dc->devices++;
}

int qz_inventory(qz_ctx_t *ctx)
{
    unsigned total_devices = 0;

    for (size_t p = 0; p < sizeof PLANES / sizeof PLANES[0]; p++) {
        const plane_t *plane = &PLANES[p];
        char service[32];
        snprintf(service, sizeof service, "%s.config", plane->proto);

        uint8_t reply[2048];
        size_t reply_len = 0;
        if (qz_request_quiet(ctx, service, QZ_OP_READ, NULL, 0, 10,
                             reply, sizeof reply, &reply_len) != 0) {
            printf("  %-7s (no answer)\n", plane->proto);
            continue;
        }
        if (reply_len == 0) {
            /* An empty snapshot encodes to zero bytes, so this is "nothing provisioned" and
             * not a failure — the distinction matters after a reboot, when it is the expected
             * answer until the host re-syncs. */
            printf("  %-7s empty\n", plane->proto);
            continue;
        }

        /* ADR-024: the board's config fingerprint. Printed because it is the fastest way to
         * answer "did this board keep its config across that reboot?" — take it before, take
         * it after, compare by eye. It is opaque: equal means the same stored config, and
         * nothing else can be read out of it. Empty means firmware without persistence. */
        char fp[32] = "";
        field_str(qz_msg_find(plane->config_msg), reply, reply_len, "config_hash", fp, sizeof fp);
        printf("  %-7s %s\n", plane->proto, fp[0] ? fp : "(not persisted)");

        device_ctx_t dc = { plane, qz_msg_find(plane->device_msg), qz_msg_find(plane->point_msg),
                            reply, reply_len, 0 };
        each_entry(reply, reply_len, FIELD_DEVICES, print_device, &dc);
        if (dc.devices == 0) printf("    (points with no device)\n");
        total_devices += dc.devices;
    }

    if (total_devices == 0) {
        qz_log("HINT", "nothing provisioned. The board holds this in RAM only, so a reboot "
                       "clears it and the host re-syncs on connect — an empty board after a "
                       "restart is normal, not a fault.");
    }
    return 0;
}
