/** json.c — print a packet as the bytes that went on the wire, and as readable JSON.
 *
 * The schema comes from the contract protos via scripts/gen-proto-tables.py, not from
 * guesswork. That matters more than it sounds: inference is wrong in ways that look right.
 * A `bytes` field holding printable ASCII prints as a string, a small enum prints as a bare
 * number, and a `double` is indistinguishable from an eight-byte blob. With declared types
 * the renderer knows, and where it does not know it says so by falling back to field numbers
 * rather than inventing a label.
 *
 * The payload is the interesting part and it is not self-describing: `RequestEnvelope.payload`
 * is `bytes`. Its real type depends on the service id and operation in the *same* envelope, so
 * both are read first and the payload is rendered as whatever the contract routes them to.
 * Not every payload is protobuf — `system.auth` carries a bare 32-byte SHA-256 proof and an
 * empty payload means logout — so those are named payload kinds rather than a decode that
 * would quietly find nothing.
 */
#include "qz.h"
#include "proto_tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The payload's type is decided by the envelope around it, so it is resolved once per packet
 * and consulted when the renderer reaches that one field. Printing is single-threaded. */
static const qz_pmsg_t  *g_env       = NULL;   /* the envelope being rendered */
static uint32_t          g_pl_field  = 0;      /* its payload field number: 8 req, 6 rsp */
static qz_payload_kind_t g_pl_kind   = QZ_PL_UNKNOWN;
static const qz_pmsg_t  *g_pl_msg    = NULL;

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

/** Does this byte range parse cleanly as a protobuf message, to the very last byte? */
static bool looks_like_message(const uint8_t *b, size_t len)
{
    if (len == 0) return false;
    size_t i = 0;
    int fields = 0;
    while (i < len) {
        uint64_t key;
        if (!read_varint(b, len, &i, &key)) return false;
        uint32_t wt = (uint32_t)(key & 7);
        if ((key >> 3) == 0) return false;             /* field 0 does not exist */
        if (wt == 0) { uint64_t v; if (!read_varint(b, len, &i, &v)) return false; }
        else if (wt == 2) {
            uint64_t l;
            if (!read_varint(b, len, &i, &l)) return false;
            if (i + l > len) return false;
            i += l;
        } else if (wt == 5) { if (i + 4 > len) return false; i += 4; }
        else if (wt == 1)   { if (i + 8 > len) return false; i += 8; }
        else return false;
        fields++;
    }
    return fields > 0;
}

static bool printable(const uint8_t *b, size_t len)
{
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++)
        if (b[i] < 0x20 || b[i] > 0x7E) return false;
    return true;
}

static void put_string(const uint8_t *b, size_t len)
{
    putchar('"');
    for (size_t i = 0; i < len; i++) {
        char c = (char)b[i];
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (b[i] >= 0x20 && b[i] <= 0x7E) putchar(c);
        else printf("\\u%04x", (unsigned)b[i]);
    }
    putchar('"');
}

static void put_hex(const uint8_t *b, size_t len)
{
    putchar('"');
    for (size_t i = 0; i < len; i++) printf("%02x", b[i]);   /* all of it, never a preview */
    putchar('"');
}

/* One occurrence of one field, collected before anything is printed so that repeated fields
 * can be emitted as JSON arrays instead of the same key over and over. */
typedef struct { uint32_t field, wt; const uint8_t *p; size_t len; uint64_t v; } item_t;

static void render_msg(const uint8_t *b, size_t len, const qz_pmsg_t *msg, int depth,
                       const char *indent);

static const qz_pfield_t *field_of(const qz_pmsg_t *msg, uint32_t number)
{
    if (msg == NULL) return NULL;
    for (int i = 0; i < msg->nfields; i++)
        if (msg->fields[i].number == number) return &msg->fields[i];
    return NULL;
}

static void put_varint_as(const qz_pfield_t *f, uint64_t v)
{
    if (f == NULL) { printf("%llu", (unsigned long long)v); return; }
    switch (f->type) {
        case QZ_T_BOOL:
            printf("%s", v ? "true" : "false");
            return;
        case QZ_T_SVARINT:
            printf("%lld", (long long)((v >> 1) ^ (~(v & 1) + 1)));
            return;
        case QZ_T_ENUM: {
            const char *lbl = qz_enum_label(f->ref, v);
            if (lbl != NULL) printf("\"%s\"", lbl);        /* readable beats a bare number */
            else printf("%llu", (unsigned long long)v);
            return;
        }
        default:
            printf("%llu", (unsigned long long)v);
    }
}

/** The payload field, rendered as whatever the envelope's service and op route it to. */
static void put_payload(const uint8_t *b, size_t len, int depth, const char *indent)
{
    switch (g_pl_kind) {
        case QZ_PL_SHA256_PROOF:
            /* Not protobuf: 32 raw bytes of sha256(nonce:client_id:sha256(password)). The key
             * names here are this tool's, not the contract's — there is no message to take
             * them from, and pretending otherwise would be the same mistake in reverse. */
            printf("{ \"_kind\": \"sha256-proof\", \"proof\": ");
            put_hex(b, len);
            printf("%s }", len == 32 ? "" : ", \"_note\": \"expected 32 bytes\"");
            return;
        case QZ_PL_EMPTY:
            if (len == 0) { printf("{}"); return; }
            printf("{ \"_note\": \"contract says empty, bytes present\", \"_bytes\": ");
            put_hex(b, len);
            printf(" }");
            return;
        case QZ_PL_MSG:
            if (len == 0) { printf("{}"); return; }
            printf("{\n");
            render_msg(b, len, g_pl_msg, depth + 1, indent);
            printf("%s%*s}", indent, depth * 2, "");
            return;
        case QZ_PL_UNKNOWN:
        default:
            /* The contract does not say what this carries. Parse it rather than assert a
             * shape, and label the fields by number so nothing is claimed that is not known. */
            if (looks_like_message(b, len)) {
                printf("{\n");
                render_msg(b, len, NULL, depth + 1, indent);
                printf("%s%*s}", indent, depth * 2, "");
            } else if (printable(b, len)) put_string(b, len);
            else put_hex(b, len);
    }
}

/** One length-delimited value, using the declared type when there is one. */
static void put_bytes_as(const qz_pfield_t *f, const qz_pmsg_t *owner, uint32_t number,
                         const uint8_t *b, size_t len, int depth, const char *indent)
{
    if (owner != NULL && owner == g_env && number == g_pl_field) {
        put_payload(b, len, depth, indent);
        return;
    }
    if (f != NULL) {
        switch (f->type) {
            case QZ_T_STRING: put_string(b, len); return;
            case QZ_T_BYTES:  put_hex(b, len);    return;
            case QZ_T_MSG:
                if (len == 0) { printf("{}"); return; }   /* an absent submessage, not a gap */
                printf("{\n");
                render_msg(b, len, (f->ref >= 0 && f->ref < qz_nmsgs) ? &qz_msgs[f->ref] : NULL,
                           depth + 1, indent);
                printf("%s%*s}", indent, depth * 2, "");
                return;
            default: break;   /* a packed repeated scalar — handled by the caller */
        }
    }
    /* No declared type: infer, and stay honest about it. */
    if (printable(b, len)) { put_string(b, len); return; }
    if (depth < 8 && looks_like_message(b, len)) {
        printf("{\n");
        render_msg(b, len, NULL, depth + 1, indent);
        printf("%s%*s}", indent, depth * 2, "");
        return;
    }
    put_hex(b, len);
}

/** proto3 packs repeated scalars into one length-delimited field; unpack it into an array. */
static void put_packed(const qz_pfield_t *f, const uint8_t *b, size_t len)
{
    printf("[");
    size_t i = 0;
    bool first = true;
    while (i < len) {
        if (!first) printf(", ");
        first = false;
        if (f->type == QZ_T_DOUBLE && i + 8 <= len) {
            double d; memcpy(&d, b + i, 8); i += 8; printf("%g", d);
        } else if ((f->type == QZ_T_FLOAT || f->type == QZ_T_FIXED32) && i + 4 <= len) {
            float g; memcpy(&g, b + i, 4); i += 4; printf("%g", (double)g);
        } else {
            uint64_t v;
            if (!read_varint(b, len, &i, &v)) break;
            put_varint_as(f, v);
        }
    }
    printf("]");
}

static void put_item(const item_t *it, const qz_pfield_t *f, const qz_pmsg_t *owner, int depth,
                     const char *indent)
{
    if (it->wt == 0) { put_varint_as(f, it->v); return; }
    if (it->wt == 1) {
        if (f != NULL && f->type == QZ_T_DOUBLE) { double d; memcpy(&d, it->p, 8); printf("%g", d); }
        else printf("%llu", (unsigned long long)it->v);
        return;
    }
    if (it->wt == 5) {
        if (f != NULL && f->type == QZ_T_FLOAT) { float g; memcpy(&g, it->p, 4); printf("%g", (double)g); }
        else printf("%llu", (unsigned long long)it->v);
        return;
    }
    if (f != NULL && f->repeated && f->type != QZ_T_MSG && f->type != QZ_T_STRING &&
        f->type != QZ_T_BYTES) {
        put_packed(f, it->p, it->len);
        return;
    }
    put_bytes_as(f, owner, it->field, it->p, it->len, depth, indent);
}

static void render_msg(const uint8_t *b, size_t len, const qz_pmsg_t *msg, int depth,
                       const char *indent)
{
    /* Collected first, printed second, so repeated fields become one JSON array rather than
     * the same key repeated — which is what the data plane sends for every point in a batch. */
    size_t cap = 32, n = 0;
    item_t *items = malloc(cap * sizeof *items);
    if (items == NULL) return;

    size_t i = 0;
    while (i < len) {
        uint64_t key;
        if (!read_varint(b, len, &i, &key)) break;
        item_t it = { (uint32_t)(key >> 3), (uint32_t)(key & 7), NULL, 0, 0 };
        if (it.field == 0) break;
        if (it.wt == 0) {
            if (!read_varint(b, len, &i, &it.v)) break;
        } else if (it.wt == 2) {
            uint64_t l;
            if (!read_varint(b, len, &i, &l) || i + l > len) break;
            it.p = b + i; it.len = (size_t)l; i += l;
        } else if (it.wt == 5) {
            if (i + 4 > len) break;
            it.p = b + i; it.len = 4; memcpy(&it.v, b + i, 4); i += 4;
        } else if (it.wt == 1) {
            if (i + 8 > len) break;
            it.p = b + i; it.len = 8; memcpy(&it.v, b + i, 8); i += 8;
        } else break;

        if (n == cap) {
            cap *= 2;
            item_t *bigger = realloc(items, cap * sizeof *items);
            if (bigger == NULL) break;
            items = bigger;
        }
        items[n++] = it;
    }

    bool *done = calloc(n ? n : 1, sizeof *done);
    if (done == NULL) { free(items); return; }

    bool first = true;
    for (size_t a = 0; a < n; a++) {
        if (done[a]) continue;
        const qz_pfield_t *f = field_of(msg, items[a].field);

        size_t count = 0;
        for (size_t c = a; c < n; c++) if (items[c].field == items[a].field) count++;

        if (!first) printf(",\n");
        first = false;
        printf("%s%*s", indent, depth * 2, "");
        if (f != NULL) printf("\"%s\": ", f->name);
        else           printf("\"%u\": ", items[a].field);

        if (count > 1) {
            printf("[");
            bool sep = false;
            for (size_t c = a; c < n; c++) {
                if (items[c].field != items[a].field) continue;
                done[c] = true;
                if (sep) printf(", ");
                sep = true;
                put_item(&items[c], f, msg, depth, indent);
            }
            printf("]");
        } else {
            done[a] = true;
            put_item(&items[a], f, msg, depth, indent);
        }
    }
    printf("\n");
    free(done);
    free(items);
}

static void dump_hex(const uint8_t *buf, size_t len, const char *indent)
{
    printf("%sencoded (%zu B)\n", indent, len);
    for (size_t i = 0; i < len; i++) {
        if (i % 32 == 0) printf("%s  ", indent);
        printf("%02x", buf[i]);
        printf("%s", ((i + 1) % 32 == 0 || i + 1 == len) ? "\n" : " ");
    }
}

static void dump_json(const uint8_t *buf, size_t len, const char *indent)
{
    printf("%sjson\n%s{\n", indent, indent);
    render_msg(buf, len, g_env, 1, indent);
    printf("%s}\n", indent);
}

/** Reads `service_id` (2) and `op` (3) out of the envelope so the payload can be routed. */
static void resolve_payload(const uint8_t *buf, size_t len, bool is_response)
{
    char service[64] = { 0 };
    uint64_t op = 0;
    size_t i = 0;
    while (i < len) {
        uint64_t key;
        if (!read_varint(buf, len, &i, &key)) break;
        uint32_t f = (uint32_t)(key >> 3), wt = (uint32_t)(key & 7);
        if (wt == 0) {
            uint64_t v;
            if (!read_varint(buf, len, &i, &v)) break;
            if (f == 3) op = v;
        } else if (wt == 2) {
            uint64_t l;
            if (!read_varint(buf, len, &i, &l) || i + l > len) break;
            if (f == 2 && l < sizeof service) memcpy(service, buf + i, (size_t)l);
            i += l;
        } else if (wt == 5) i += 4;
        else if (wt == 1)   i += 8;
        else break;
    }
    g_pl_kind = qz_payload_type(service[0] ? service : NULL, op, is_response, &g_pl_msg);
}

void qz_packet_dump(const uint8_t *buf, size_t len, bool is_response, const char *indent)
{
    g_env      = qz_msg_find(is_response ? "ResponseEnvelope" : "RequestEnvelope");
    g_pl_field = is_response ? 6 : 8;    /* the envelopes are not symmetric here */
    resolve_payload(buf, len, is_response);

    /* Ordered the way each direction actually happens, so the listing reads as a sequence of
     * events rather than two unrelated views: a request is composed and then encoded, a reply
     * arrives encoded and is then decoded. */
    if (!is_response) { dump_json(buf, len, indent); dump_hex(buf, len, indent); }
    else              { dump_hex(buf, len, indent);  dump_json(buf, len, indent); }

    g_env = NULL; g_pl_msg = NULL; g_pl_kind = QZ_PL_UNKNOWN;
}

const char *qz_error_name(uint64_t code)
{
    for (int i = 0; i < qz_nenums; i++) {
        if (strstr(qz_enums[i].name, "ErrorCode") == NULL) continue;
        for (int v = 0; v < qz_enums[i].nvals; v++)
            if (qz_enums[i].vals[v].value == code) return qz_enums[i].vals[v].name;
    }
    return "ERROR_?";
}
