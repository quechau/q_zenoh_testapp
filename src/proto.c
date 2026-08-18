/** proto.c — just enough protobuf for the ce.embedded envelopes.
 *
 * Hand-rolled on purpose: the app then has no protoc, no grpc and no generated sources to
 * keep in step. Field numbers were verified against ACB-M's generated
 * components/app_zenoh/codec/envelope.pb.h, which matches proto/envelope.proto here.
 *
 *   RequestEnvelope   1 wire_version  2 service_id  3 op  4 seq  5 client_id
 *                     6 deadline_ms   7 encoding    8 payload
 *   ResponseEnvelope  1 wire_version  2 service_id  3 op  4 seq  5 encoding
 *                     6 payload       7 error       8 is_notification
 *
 * The two are NOT symmetric — request payload is field 8, response payload is field 6. Sending
 * a payload as field 7 puts it where the request expects `encoding`, a varint enum; nanopb
 * then fails the whole decode, the board resets the envelope to zero, and the request arrives
 * with an empty client_id and is refused as a certificate mismatch. Easy to chase in the wrong
 * direction, so: check the tags, not the shape.
 */
#include "qz.h"

#include <stdio.h>
#include <string.h>

static const char *REQ_FIELDS[] = {
    "?", "wire_version", "service_id", "op", "seq", "client_id", "deadline_ms",
    "encoding", "payload"
};
static const char *RSP_FIELDS[] = {
    "?", "wire_version", "service_id", "op", "seq", "encoding", "payload", "error",
    "is_notification"
};

static size_t put_varint(uint8_t *buf, uint64_t v)
{
    size_t n = 0;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        buf[n++] = b | (v != 0 ? 0x80 : 0);
    } while (v != 0);
    return n;
}

static bool get_varint(const uint8_t *buf, size_t len, size_t *i, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (*i < len) {
        uint8_t b = buf[(*i)++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) { *out = v; return true; }
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

int qz_req_encode(uint8_t *buf, size_t buf_len, const char *service_id, qz_op_t op,
                  uint32_t seq, const char *client_id,
                  const uint8_t *payload, size_t payload_len)
{
    size_t n = 0;
    #define NEED(x) do { if (n + (x) > buf_len) return -1; } while (0)

    NEED(2); buf[n++] = (1 << 3) | 0; n += put_varint(buf + n, 1);          /* wire_version */

    size_t sl = strlen(service_id);
    NEED(2 + sl); buf[n++] = (2 << 3) | 2; n += put_varint(buf + n, sl);
    memcpy(buf + n, service_id, sl); n += sl;                                /* service_id */

    NEED(2); buf[n++] = (3 << 3) | 0; n += put_varint(buf + n, (uint64_t)op);/* op */
    NEED(6); buf[n++] = (4 << 3) | 0; n += put_varint(buf + n, seq);         /* seq */

    size_t cl = strlen(client_id);
    NEED(2 + cl); buf[n++] = (5 << 3) | 2; n += put_varint(buf + n, cl);
    memcpy(buf + n, client_id, cl); n += cl;                                 /* client_id */

    if (payload != NULL && payload_len > 0) {
        NEED(6 + payload_len); buf[n++] = (8 << 3) | 2;   /* payload is field 8 */
        n += put_varint(buf + n, payload_len);
        memcpy(buf + n, payload, payload_len); n += payload_len;             /* payload */
    }
    #undef NEED
    return (int)n;
}


bool qz_field_varint(const uint8_t *buf, size_t len, uint32_t field, uint64_t *out)
{
    size_t i = 0;
    while (i < len) {
        uint64_t key;
        if (!get_varint(buf, len, &i, &key)) return false;
        uint32_t f = (uint32_t)(key >> 3);
        uint32_t wt = (uint32_t)(key & 7);
        if (wt == 0) {
            uint64_t v;
            if (!get_varint(buf, len, &i, &v)) return false;
            if (f == field) { *out = v; return true; }
        } else if (wt == 2) {
            uint64_t l;
            if (!get_varint(buf, len, &i, &l)) return false;
            i += l;
        } else if (wt == 5) i += 4;
        else if (wt == 1)   i += 8;
        else return false;
    }
    return false;
}

size_t qz_field_bytes(const uint8_t *buf, size_t len, uint32_t field, const uint8_t **out)
{
    size_t i = 0;
    while (i < len) {
        uint64_t key;
        if (!get_varint(buf, len, &i, &key)) return 0;
        uint32_t f = (uint32_t)(key >> 3);
        uint32_t wt = (uint32_t)(key & 7);
        if (wt == 2) {
            uint64_t l;
            if (!get_varint(buf, len, &i, &l)) return 0;
            if (i + l > len) return 0;
            if (f == field) { *out = buf + i; return (size_t)l; }
            i += l;
        } else if (wt == 0) { uint64_t v; if (!get_varint(buf, len, &i, &v)) return 0; }
        else if (wt == 5) i += 4;
        else if (wt == 1) i += 8;
        else return 0;
    }
    return 0;
}

const char *qz_op_name(qz_op_t op)
{
    switch (op) {
        case QZ_OP_READ:      return "READ";
        case QZ_OP_WRITE:     return "WRITE";
        case QZ_OP_VALIDATE:  return "VALIDATE";
        case QZ_OP_SUBSCRIBE: return "SUBSCRIBE";
        case QZ_OP_EXECUTE:   return "EXECUTE";
        case QZ_OP_DISCOVER:  return "DISCOVER";
        case QZ_OP_PING:      return "PING";
        case QZ_OP_RESET:     return "RESET";
        default:              return "OP?";
    }
}
