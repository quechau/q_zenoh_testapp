/** flow.c — system.flow: upload a .riot application, ask what is running (contract 0.3.0).
 *
 * The debuggable path that exists BEFORE the Studio compiler does (roadmap Phase 3): a
 * hand-written .riot uploaded from here proves the transport end to end, so when the compiler
 * lands there is never a question of which half is broken.
 *
 * The FlowWrite payloads are hand-encoded rather than built through the schema tables: they
 * nest messages inside a oneof, and the varint+length-delimited encoding is six lines — less
 * machinery than teaching qz_build about submessages for one caller.
 */
#include "qz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FLOW_CHUNK 384u

static size_t enc_varint(uint8_t *o, uint64_t v)
{
    size_t n = 0;
    do { o[n++] = (uint8_t)((v & 0x7F) | (v > 0x7F ? 0x80 : 0)); v >>= 7; } while (v);
    return n;
}
static size_t enc_key(uint8_t *o, uint32_t field, uint32_t wt) { return enc_varint(o, (field << 3) | wt); }
static size_t enc_ld(uint8_t *o, uint32_t field, const uint8_t *b, size_t len)
{
    size_t n = enc_key(o, field, 2);
    n += enc_varint(o + n, len);
    memcpy(o + n, b, len);
    return n + len;
}

/** One FlowWrite phase, sent and checked. Returns 0 on OK. */
static int flow_send(qz_ctx_t *ctx, const uint8_t *inner, size_t inner_len, uint32_t phase_field,
                     uint8_t *reply, size_t *reply_len)
{
    uint8_t msg[FLOW_CHUNK + 32];
    size_t n = enc_ld(msg, phase_field, inner, inner_len);   /* FlowWrite.{begin|data|commit} */
    return qz_request_quiet(ctx, "system.flow", QZ_OP_WRITE, msg, n, 10,
                            reply, 1536, reply_len);
}

static void flow_print_status(const uint8_t *payload, size_t len)
{
    /* FlowStatus: 1 sha(str) 2 running(varint) 3 node_count 4 error(str) 5 received */
    char sha[80] = "", err[100] = "";
    unsigned long long running = 0, nodes = 0, received = 0;
    size_t i = 0;
    while (i < len) {
        uint64_t key = 0; int sh = 0;
        while (i < len) { uint8_t c = payload[i++]; key |= (uint64_t)(c & 0x7F) << sh; if (!(c & 0x80)) break; sh += 7; }
        uint32_t f = (uint32_t)(key >> 3), wt = key & 7;
        if (wt == 0) {
            uint64_t v = 0; sh = 0;
            while (i < len) { uint8_t c = payload[i++]; v |= (uint64_t)(c & 0x7F) << sh; if (!(c & 0x80)) break; sh += 7; }
            if (f == 2) running = v; else if (f == 3) nodes = v; else if (f == 5) received = v;
        } else if (wt == 2) {
            uint64_t l = 0; sh = 0;
            while (i < len) { uint8_t c = payload[i++]; l |= (uint64_t)(c & 0x7F) << sh; if (!(c & 0x80)) break; sh += 7; }
            if (i + l > len) return;
            if (f == 1) { size_t c = l < sizeof sha - 1 ? l : sizeof sha - 1; memcpy(sha, payload + i, c); sha[c] = 0; }
            if (f == 4) { size_t c = l < sizeof err - 1 ? l : sizeof err - 1; memcpy(err, payload + i, c); err[c] = 0; }
            i += l;
        } else return;
    }
    printf("  flow: sha=%s\n        running=%s  nodes=%llu  received=%llu%s%s\n",
           sha[0] ? sha : "(none)", running ? "yes" : "no", nodes, received,
           err[0] ? "\n        error: " : "", err);
}

int qz_flow_status(qz_ctx_t *ctx)
{
    uint8_t reply[1536]; size_t rl = 0;
    if (qz_request_quiet(ctx, "system.flow", QZ_OP_READ, NULL, 0, 10, reply, sizeof reply, &rl) != 0)
        return 1;
    flow_print_status(reply, rl);
    return 0;
}

int qz_flow_put(qz_ctx_t *ctx, const char *path, bool corrupt)
{
    return qz_flow_put_opts(ctx, path, corrupt, 0);
}

/* slow_ms > 0 sleeps between chunks — the test hook that makes a mid-upload kill or a
 * begin-over-begin reproducible instead of a race against a ~1 s transfer. */
int qz_flow_put_opts(qz_ctx_t *ctx, const char *path, bool corrupt, unsigned slow_ms)
{
    FILE *f = fopen(path, "rb");
    if (!f) { qz_log("ERR", "cannot open %s", path); return 1; }
    static uint8_t file[8192];
    size_t size = fread(file, 1, sizeof file, f);
    fclose(f);
    if (size < 12) { qz_log("ERR", "%s: %zu bytes is not a .riot file", path, size); return 1; }

    uint8_t digest[32]; char sha[65];
    qz_sha256(file, size, digest);
    for (int i = 0; i < 32; i++) sprintf(&sha[i * 2], "%02x", digest[i]);
    qz_log("FLOW", "%s: %zu bytes sha=%.16s… (%zu chunks)%s", path, size, sha,
           (size + FLOW_CHUNK - 1) / FLOW_CHUNK, corrupt ? " [CORRUPTING chunk 0]" : "");

    uint8_t reply[1536]; size_t rl;
    /* begin: FlowBegin{1:size 2:sha} */
    uint8_t inner[128]; size_t n = 0;
    n += enc_key(inner + n, 1, 0); n += enc_varint(inner + n, size);
    n += enc_ld(inner + n, 2, (const uint8_t *)sha, 64);
    if (flow_send(ctx, inner, n, 1, reply, &rl) != 0) { qz_log("ERR", "begin failed"); return 1; }

    for (size_t off = 0; off < size; off += FLOW_CHUNK) {
        size_t c = size - off < FLOW_CHUNK ? size - off : FLOW_CHUNK;
        uint8_t data[FLOW_CHUNK + 24]; n = 0;
        n += enc_key(data + n, 1, 0); n += enc_varint(data + n, off);
        uint8_t byte0 = file[off];
        if (corrupt && off == 0) file[0] ^= 0xFF;          /* the acceptance test's sabotage */
        n += enc_ld(data + n, 2, file + off, c);
        file[off] = byte0;
        if (flow_send(ctx, data, n, 2, reply, &rl) != 0) { qz_log("ERR", "data@%zu failed", off); return 1; }
        if (slow_ms) { struct timespec ts = { slow_ms / 1000, (slow_ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
    }

    /* commit: FlowWrite{3: FlowCommit{}} — the reply carries the verdict either way */
    int rc = flow_send(ctx, NULL, 0, 3, reply, &rl);
    flow_print_status(reply, rl);
    if (rc != 0) { qz_log("FLOW", "commit REFUSED (see error above) — old flow untouched"); return 1; }
    qz_log("FLOW", "committed — board persists and restarts in 3 s to load it");
    return 0;
}
