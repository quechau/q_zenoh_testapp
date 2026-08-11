/** json.c — render a protobuf message as readable JSON, without a schema.
 *
 * The field-by-field dump in proto.c shows the wire truth; this shows what it *means*. Nested
 * payloads matter most: `system.boardinfo` answers with 141 bytes of nested protobuf, which as
 * a hex preview tells you nothing.
 *
 * There is no generated code here, so the renderer infers rather than knows:
 *   - varint fields print as numbers;
 *   - length-delimited fields print as strings when the bytes are printable, otherwise the
 *     renderer tries to parse them as a nested message and prints an object if that succeeds;
 *   - anything else prints as a hex string.
 *
 * Field NAMES come from a small table per message type, so the common answers read properly.
 * Fields with no entry keep their number ("7"), which keeps the output honest about what is
 * actually known rather than inventing labels.
 */
#include "qz.h"

#include <stdio.h>
#include <string.h>

typedef struct { uint32_t field; const char *name; } qz_name_t;

/* The reply payload is a wrapper: GetBoardInfoResponse { BoardInfo info = 1; }. Naming the
 * top level correctly matters — applying BoardInfo's table one level too high labelled the
 * whole object "board_name", which reads like a decoded value and is not one. */
static const qz_name_t BOARDINFO_RESPONSE[] = { {1, "info"}, {0, NULL} };

/* rubix.embedded.v1.BoardInfo — proto/services/service_boardinfo.proto */
static const qz_name_t BOARDINFO[] = {
    {1, "board_name"}, {2, "board_type"}, {3, "chip_type"}, {4, "hardware_version"},
    {5, "software_version"}, {6, "firmware_version"}, {7, "protocol_version"},
    {8, "build_time"}, {9, "supported_services"}, {10, "supported_transports"},
    {11, "supported_interfaces"}, {0, NULL}
};
/* The repeated entries inside BoardInfo. Both start with an id and a version, which is enough
 * to make the list readable. */
static const qz_name_t SUPPORTED[] = {
    {1, "id"}, {2, "version"}, {4, "healthy"}, {5, "capability_hash"},
    {6, "is_simulated"}, {7, "swappable"}, {0, NULL}
};

/** Which table applies one level down from `names` at `field`. */
static const qz_name_t *descend(const qz_name_t *names, uint32_t field)
{
    if (names == BOARDINFO_RESPONSE && field == 1) return BOARDINFO;
    if (names == BOARDINFO && field >= 9 && field <= 11) return SUPPORTED;
    return names;
}

static const char *lookup(const qz_name_t *tbl, uint32_t field)
{
    for (size_t i = 0; tbl != NULL && tbl[i].name != NULL; i++)
        if (tbl[i].field == field) return tbl[i].name;
    return NULL;
}

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

static void put_json_string(const uint8_t *b, size_t len)
{
    putchar('"');
    for (size_t i = 0; i < len; i++) {
        char c = (char)b[i];
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else putchar(c);
    }
    putchar('"');
}

static void render(const uint8_t *b, size_t len, const qz_name_t *names, int depth,
                   const char *indent);

/** Prints one field's value. `names` applies to a nested message, if this turns out to be one. */
static void render_value(uint32_t field, uint32_t wt, const uint8_t *b, size_t len,
                         const qz_name_t *names, int depth, const char *indent)
{
    (void)field;
    if (wt == 2) {
        if (printable(b, len)) { put_json_string(b, len); return; }
        if (depth < 6 && looks_like_message(b, len)) {
            printf("{\n");
            render(b, len, names, depth + 1, indent);
            printf("%*s}", (depth) * 2 + (int)strlen(indent), "");
            return;
        }
        printf("\"");
        for (size_t i = 0; i < len && i < 64; i++) printf("%02x", b[i]);
        printf("%s\"", len > 64 ? "…" : "");
        return;
    }
    printf("null");
}

static void render(const uint8_t *b, size_t len, const qz_name_t *names, int depth,
                   const char *indent)
{
    size_t i = 0;
    bool first = true;
    while (i < len) {
        uint64_t key;
        if (!read_varint(b, len, &i, &key)) break;
        uint32_t field = (uint32_t)(key >> 3);
        uint32_t wt    = (uint32_t)(key & 7);

        if (!first) printf(",\n");
        first = false;
        printf("%s%*s", indent, depth * 2, "");

        const char *name = lookup(names, field);
        if (name != NULL) printf("\"%s\": ", name);
        else              printf("\"%u\": ", field);

        if (wt == 0) {
            uint64_t v;
            if (!read_varint(b, len, &i, &v)) break;
            printf("%llu", (unsigned long long)v);
        } else if (wt == 2) {
            uint64_t l;
            if (!read_varint(b, len, &i, &l)) break;
            if (i + l > len) break;
            render_value(field, wt, b + i, (size_t)l, descend(names, field), depth, indent);
            i += l;
        } else if (wt == 5) { i += 4; printf("null"); }
        else if (wt == 1)   { i += 8; printf("null"); }
        else break;
    }
    printf("\n");
}

void qz_json_dump(const uint8_t *buf, size_t len, const char *service, const char *indent)
{
    /* Only the payload is rendered: the envelope itself is already printed field by field, and
     * repeating it as JSON would say nothing new. */
    const qz_name_t *names = NULL;
    if (service != NULL && strcmp(service, "system.boardinfo") == 0) names = BOARDINFO_RESPONSE;

    printf("%s{\n", indent);
    render(buf, len, names, 1, indent);
    printf("%s}\n", indent);
}
