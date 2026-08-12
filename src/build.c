/** build.c — turn `field=value` arguments into a protobuf payload, using the contract schema.
 *
 * Until this existed the tool could only send empty payloads, so every request it could make
 * was a READ. Anything that carries data — writing a point, provisioning a device — was
 * undocumentable and untestable, which is a poor state for a tool whose whole job is to
 * exercise a board.
 *
 * Nothing here knows what a Modbus register or a BACnet object is. It looks the field up by
 * NAME in the generated schema and encodes it by its DECLARED type, so `scale=1.5` becomes a
 * fixed32 float and `reg_type=REG_HOLDING` becomes varint 4 — and the same code serves modbus,
 * bacnet and lora without a line of per-protocol handling. A name that is not in the .proto is
 * refused with the list of names that are, rather than silently encoded as something else.
 */
#include "qz.h"
#include "proto_tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t put_varint(uint8_t *out, uint64_t v)
{
    size_t n = 0;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        out[n++] = v ? (uint8_t)(b | 0x80) : b;
    } while (v);
    return n;
}

static size_t put_key(uint8_t *out, uint32_t field, uint32_t wt)
{
    return put_varint(out, ((uint64_t)field << 3) | wt);
}

/** Parses a boolean the way an operator would type one. */
static bool parse_bool(const char *s, bool *out)
{
    if (strcmp(s, "true") == 0 || strcmp(s, "1") == 0 || strcmp(s, "yes") == 0)  { *out = true;  return true; }
    if (strcmp(s, "false") == 0 || strcmp(s, "0") == 0 || strcmp(s, "no") == 0)  { *out = false; return true; }
    return false;
}

/** An enum value given by its contract name ("REG_HOLDING") or by its number. */
static bool parse_enum(int ref, const char *s, uint64_t *out)
{
    if (ref >= 0 && ref < qz_nenums) {
        for (int i = 0; i < qz_enums[ref].nvals; i++)
            if (strcmp(qz_enums[ref].vals[i].name, s) == 0) { *out = qz_enums[ref].vals[i].value; return true; }
    }
    if (s[0] >= '0' && s[0] <= '9') { *out = strtoull(s, NULL, 0); return true; }
    return false;
}

static void list_fields(const qz_pmsg_t *msg)
{
    qz_log("HINT", "%s accepts:", msg->name);
    for (int i = 0; i < msg->nfields; i++) {
        const qz_pfield_t *f = &msg->fields[i];
        if (f->type == QZ_T_ENUM && f->ref >= 0 && f->ref < qz_nenums) {
            printf("              %-14s enum:", f->name);
            for (int v = 0; v < qz_enums[f->ref].nvals; v++)
                printf(" %s", qz_enums[f->ref].vals[v].name);
            printf("\n");
        } else {
            static const char *kind[] = { "uint", "sint", "bool", "double", "float",
                                          "fixed32", "fixed64", "string", "bytes(hex)",
                                          "message", "enum" };
            printf("              %-14s %s\n", f->name, kind[f->type]);
        }
    }
}

int qz_build(const struct qz_pmsg *msg_, int argc, char **argv, uint8_t *out, size_t cap, size_t *len)
{
    const qz_pmsg_t *msg = (const qz_pmsg_t *)msg_;
    size_t n = 0;

    if (msg == NULL) { qz_log("ERR", "no such message in the contract"); return -1; }

    for (int a = 0; a < argc; a++) {
        const char *eq = strchr(argv[a], '=');
        if (eq == NULL) {
            qz_log("ERR", "expected field=value, got '%s'", argv[a]);
            list_fields(msg);
            return -1;
        }
        size_t klen = (size_t)(eq - argv[a]);
        const char *val = eq + 1;

        const qz_pfield_t *f = NULL;
        for (int i = 0; i < msg->nfields; i++)
            if (strncmp(msg->fields[i].name, argv[a], klen) == 0 &&
                msg->fields[i].name[klen] == '\0') { f = &msg->fields[i]; break; }
        if (f == NULL) {
            qz_log("ERR", "'%.*s' is not a field of %s", (int)klen, argv[a], msg->name);
            list_fields(msg);
            return -1;
        }
        if (n + 32 + strlen(val) > cap) { qz_log("ERR", "payload too large"); return -1; }

        switch (f->type) {
            case QZ_T_STRING:
            case QZ_T_BYTES: {
                size_t vlen = strlen(val);
                uint8_t tmp[512];
                const uint8_t *src = (const uint8_t *)val;
                if (f->type == QZ_T_BYTES) {          /* bytes are given as hex */
                    if (vlen % 2 != 0 || vlen / 2 > sizeof tmp) {
                        qz_log("ERR", "%s takes hex bytes", f->name);
                        return -1;
                    }
                    for (size_t i = 0; i < vlen / 2; i++) {
                        unsigned byte;
                        if (sscanf(val + i * 2, "%2x", &byte) != 1) {
                            qz_log("ERR", "%s: bad hex", f->name);
                            return -1;
                        }
                        tmp[i] = (uint8_t)byte;
                    }
                    src = tmp;
                    vlen /= 2;
                }
                n += put_key(out + n, f->number, 2);
                n += put_varint(out + n, vlen);
                memcpy(out + n, src, vlen);
                n += vlen;
                break;
            }
            case QZ_T_FLOAT: {
                float g = strtof(val, NULL);
                n += put_key(out + n, f->number, 5);
                memcpy(out + n, &g, 4);
                n += 4;
                break;
            }
            case QZ_T_DOUBLE: {
                double d = strtod(val, NULL);
                n += put_key(out + n, f->number, 1);
                memcpy(out + n, &d, 8);
                n += 8;
                break;
            }
            case QZ_T_BOOL: {
                bool b;
                if (!parse_bool(val, &b)) { qz_log("ERR", "%s takes true/false", f->name); return -1; }
                n += put_key(out + n, f->number, 0);
                n += put_varint(out + n, b ? 1 : 0);
                break;
            }
            case QZ_T_ENUM: {
                uint64_t v;
                if (!parse_enum(f->ref, val, &v)) {
                    qz_log("ERR", "'%s' is not a value of %s", val, f->name);
                    list_fields(msg);
                    return -1;
                }
                n += put_key(out + n, f->number, 0);
                n += put_varint(out + n, v);
                break;
            }
            case QZ_T_SVARINT: {
                int64_t s = strtoll(val, NULL, 0);
                n += put_key(out + n, f->number, 0);
                n += put_varint(out + n, ((uint64_t)s << 1) ^ (uint64_t)(s >> 63));
                break;
            }
            case QZ_T_MSG:
                qz_log("ERR", "%s is a nested message — not settable this way", f->name);
                return -1;
            default: {
                n += put_key(out + n, f->number, 0);
                n += put_varint(out + n, strtoull(val, NULL, 0));
                break;
            }
        }
    }
    *len = n;
    return 0;
}

size_t qz_wrap(uint8_t *out, size_t cap, uint32_t field, const uint8_t *sub, size_t sublen)
{
    size_t n = 0;
    if (sublen + 16 > cap) return 0;
    n += put_key(out + n, field, 2);
    n += put_varint(out + n, sublen);
    memcpy(out + n, sub, sublen);
    return n + sublen;
}

size_t qz_packed_u32(uint8_t *out, size_t cap, uint32_t field, const uint32_t *ids, size_t count)
{
    uint8_t body[256];
    size_t b = 0;
    for (size_t i = 0; i < count && b + 8 < sizeof body; i++) b += put_varint(body + b, ids[i]);
    return qz_wrap(out, cap, field, body, b);
}
