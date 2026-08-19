/** session.c — the zenoh side: open a peer session over mTLS, discover boards, pub/sub,
 *  log in with the S1 proof, and run a protobuf request/reply.
 */
#include "qz.h"
#include "zenoh-pico/net/session.h"   /* _zp_send_keep_alive — see qz_session_ensure */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ session */

int qz_session_open(qz_ctx_t *ctx)
{
    if (ctx->session_open) qz_session_close(ctx);

    char ca[QZ_MAX_PATH], cert[QZ_MAX_PATH], key[QZ_MAX_PATH];
    snprintf(ca,   sizeof(ca),   "%s/ca.pem",        ctx->certs_dir);
    snprintf(cert, sizeof(cert), "%s/device.pem",    ctx->certs_dir);
    snprintf(key,  sizeof(key),  "%s/device-key.pem", ctx->certs_dir);

    /* ADR-016: the board compares the envelope client_id against the CN of the certificate on
     * this session and refuses everything on a mismatch. Say so up front rather than letting
     * every later command fail with a confusing PERMISSION denied. */
    char cn[QZ_MAX_ID] = {0};
    if (qz_cert_cn(cert, cn, sizeof(cn)) == 0) {
        if (ctx->client_id[0] == '\0') {
            snprintf(ctx->client_id, sizeof(ctx->client_id), "%s", cn);
            qz_log("ID", "client_id taken from the certificate CN: %s", ctx->client_id);
        } else if (strcmp(cn, ctx->client_id) != 0) {
            qz_log("WARN", "client_id '%s' != certificate CN '%s' — the board will answer "
                           "PERMISSION denied to every request (ADR-016)",
                   ctx->client_id, cn);
        }
    } else {
        qz_log("WARN", "could not read the CN from %s", cert);
    }

    z_owned_config_t cfg;
    z_config_default(&cfg);
    zp_config_insert(z_loan_mut(cfg), Z_CONFIG_MODE_KEY, "peer");

    /* Order matters. zenoh-pico's config map has 16 buckets and hashes the raw key;
     * Z_CONFIG_CONNECT_KEY (0x41) and Z_CONFIG_TLS_ENABLE_MTLS_KEY (0x51) both land in
     * bucket 1, and _z_config_get_all walks a bucket without re-checking the key. Insert the
     * CONNECT locator FIRST or the literal "true" from enable_mtls is handed to the locator
     * parser as if it were an endpoint. */
    zp_config_insert(z_loan_mut(cfg), Z_CONFIG_CONNECT_KEY, ctx->endpoint);
    if (ctx->listen[0] != '\0')
        zp_config_insert(z_loan_mut(cfg), Z_CONFIG_LISTEN_KEY, ctx->listen);

    zp_config_insert(z_loan_mut(cfg), Z_CONFIG_TLS_ROOT_CA_CERTIFICATE_KEY, ca);
    zp_config_insert(z_loan_mut(cfg), Z_CONFIG_TLS_CONNECT_CERTIFICATE_KEY, cert);
    zp_config_insert(z_loan_mut(cfg), Z_CONFIG_TLS_CONNECT_PRIVATE_KEY_KEY, key);
    if (ctx->listen[0] != '\0') {
        zp_config_insert(z_loan_mut(cfg), Z_CONFIG_TLS_LISTEN_CERTIFICATE_KEY, cert);
        zp_config_insert(z_loan_mut(cfg), Z_CONFIG_TLS_LISTEN_PRIVATE_KEY_KEY, key);
    }
    zp_config_insert(z_loan_mut(cfg), Z_CONFIG_TLS_ENABLE_MTLS_KEY, "true");
    /* The board's certificate pins the address it was enrolled at, so verification of the
     * name fails whenever it is reached by any other address. Off by default here, on with
     * --verify-name when you do want to check it. */
    zp_config_insert(z_loan_mut(cfg), Z_CONFIG_TLS_VERIFY_NAME_ON_CONNECT_KEY,
                     ctx->verify_name ? "true" : "false");

    uint64_t t0 = qz_now_ms();
    if (z_open(&ctx->session, z_move(cfg), NULL) != Z_OK) {
        qz_log("FATAL", "z_open failed for %s — is the board listening, and does its CA match "
                        "%s?", ctx->endpoint, ca);
        return -1;
    }
    if (zp_start_read_task(z_loan_mut(ctx->session), NULL) != Z_OK ||
        zp_start_lease_task(z_loan_mut(ctx->session), NULL) != Z_OK) {
        qz_log("FATAL", "could not start the zenoh read/lease tasks");
        z_drop(z_move(ctx->session));
        return -1;
    }
    ctx->session_open = true;
    ctx->logged_in = false;
    qz_log("OPEN", "session up in %llu ms  endpoint=%s  client_id=%s",
           (unsigned long long)(qz_now_ms() - t0), ctx->endpoint, ctx->client_id);
    return 0;
}

void qz_session_close(qz_ctx_t *ctx)
{
    if (!ctx->session_open) return;
    z_drop(z_move(ctx->session));
    ctx->session_open = false;
    ctx->logged_in = false;
    qz_log("CLOSE", "session closed");
}

/* ---------------------------------------------------------------- discovery */

static void on_announce(z_loaned_sample_t *sample, void *arg)
{
    qz_ctx_t *ctx = (qz_ctx_t *)arg;

    z_view_string_t ks;
    if (z_keyexpr_as_view_string(z_sample_keyexpr(sample), &ks) != Z_OK) return;
    char key[QZ_MAX_KEY];
    size_t kl = z_string_len(z_loan(ks));
    if (kl >= sizeof(key)) kl = sizeof(key) - 1;
    memcpy(key, z_string_data(z_loan(ks)), kl);
    key[kl] = '\0';

    /* ce/peers/<peer-id>/announce */
    const char *p = strstr(key, "ce/peers/");
    if (p == NULL) return;
    p += strlen("ce/peers/");
    const char *slash = strchr(p, '/');
    if (slash == NULL) return;
    char id[QZ_MAX_ID];
    size_t idl = (size_t)(slash - p);
    if (idl >= sizeof(id)) return;
    memcpy(id, p, idl);
    id[idl] = '\0';

    /* Payload is "<seq> <nonce-hex>" — the nonce is what `login` needs, and it changes on
     * every board reboot, so it is re-read here rather than cached across runs. */
    char nonce[QZ_NONCE_HEX] = {0};
    z_owned_slice_t slice;
    if (z_bytes_to_slice(z_sample_payload(sample), &slice) == Z_OK) {
        const uint8_t *d = z_slice_data(z_loan(slice));
        size_t n = z_slice_len(z_loan(slice));
        char tmp[128];
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        memcpy(tmp, d, n);
        tmp[n] = '\0';
        const char *sp = strchr(tmp, ' ');
        if (sp != NULL) snprintf(nonce, sizeof(nonce), "%s", sp + 1);
        z_drop(z_move(slice));
    }

    for (size_t i = 0; i < ctx->board_count; i++) {
        if (strcmp(ctx->boards[i].peer_id, id) == 0) {
            ctx->boards[i].announces++;
            ctx->boards[i].last_seen_ms = qz_now_ms();
            if (nonce[0] != '\0') snprintf(ctx->boards[i].nonce, QZ_NONCE_HEX, "%s", nonce);
            return;
        }
    }
    if (ctx->board_count < QZ_MAX_BOARDS) {
        qz_board_t *b = &ctx->boards[ctx->board_count++];
        snprintf(b->peer_id, sizeof(b->peer_id), "%s", id);
        snprintf(b->nonce, sizeof(b->nonce), "%s", nonce);
        b->announces = 1;
        b->last_seen_ms = qz_now_ms();
        qz_log("FOUND", "%s  (nonce %s)", b->peer_id, b->nonce[0] ? b->nonce : "not seen yet");
    }
}

/* A board reboot silently kills the TCP link. The lease task eventually marks
 * the session closed, but until 2026-08-19 every later command just timed out
 * on the corpse and the no-reply HINT blamed old firmware — a live debug
 * session went exactly down that wrong path. Detect the corpse up front and
 * reconnect. Auth is per-session, so authorized services need a fresh
 * `login` afterwards — say so instead of leaving it to be rediscovered. */
int qz_session_ensure(qz_ctx_t *ctx)
{
    if (!ctx->session_open) return -1;
    /* is_closed alone is NOT enough: measured 2026-08-19, a board reboot leaves
     * the TLS link erroring (-0x004e on every write) yet the session object
     * still reports open for minutes. A keep-alive probe forces a write NOW and
     * surfaces the corpse immediately. */
    bool b_dead = z_session_is_closed(z_loan(ctx->session));
    if (!b_dead) {
        /* zp_send_keep_alive() is compiled out under Z_FEATURE_MULTI_THREAD, so
         * reach one layer down (same call the wrapper makes — api.c:2536). */
        b_dead = _zp_send_keep_alive(_Z_RC_IN_VAL(z_loan(ctx->session))) != Z_OK;
    }
    if (!b_dead) return 0;
    qz_log("HINT", "the session is DEAD — the board likely rebooted and cut the TCP link "
                   "(a reboot also clears its RAM config). Reconnecting...");
    qz_session_close(ctx);
    if (qz_session_open(ctx) != 0) {
        qz_log("HINT", "reopen failed — is the board back on the network? try `scan`.");
        return -1;
    }
    qz_log("HINT", "session reopened. Authorized services need `login <password>` again "
                   "(auth is per-session).");
    return 0;
}

int qz_discover(qz_ctx_t *ctx, unsigned seconds)
{
    if (!ctx->session_open) { qz_log("ERR", "no session — run `connect` first"); return -1; }

    ctx->board_count = 0;
    z_owned_closure_sample_t closure;
    z_closure_sample(&closure, on_announce, NULL, ctx);
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, "ce/peers/*/announce");
    z_owned_subscriber_t sub;
    if (z_declare_subscriber(z_loan(ctx->session), &sub, z_loan(ke), z_move(closure), NULL)
        != Z_OK) {
        qz_log("ERR", "could not subscribe to the announce beacon");
        return -1;
    }
    if (qz_session_ensure(ctx) != 0) return -1;
    qz_log("SCAN", "listening %us for ce/peers/*/announce", seconds);
    sleep(seconds);
    z_drop(z_move(sub));

    if (ctx->board_count == 0) {
        qz_log("SCAN", "no board announced. The session is up, so either nothing is "
                       "publishing or this peer is not reaching it.");
        return 0;
    }
    printf("\n  %-28s %9s  %s\n", "PEER ID", "ANNOUNCES", "NONCE");
    for (size_t i = 0; i < ctx->board_count; i++)
        printf("  %-28s %9u  %s\n", ctx->boards[i].peer_id, ctx->boards[i].announces,
               ctx->boards[i].nonce);
    printf("\n");
    if (ctx->board[0] == '\0' && ctx->board_count == 1) {
        snprintf(ctx->board, sizeof(ctx->board), "%s", ctx->boards[0].peer_id);
        qz_log("SELECT", "only one board seen — commands will address %s", ctx->board);
    }
    return (int)ctx->board_count;
}

/* ------------------------------------------------------------------ pub/sub */

typedef struct { unsigned count; bool dump; } sub_state_t;

static void on_any(z_loaned_sample_t *sample, void *arg)
{
    sub_state_t *st = (sub_state_t *)arg;
    st->count++;

    z_view_string_t ks;
    if (z_keyexpr_as_view_string(z_sample_keyexpr(sample), &ks) != Z_OK) return;
    z_owned_slice_t slice;
    size_t n = 0;
    const uint8_t *d = NULL;
    if (z_bytes_to_slice(z_sample_payload(sample), &slice) == Z_OK) {
        d = z_slice_data(z_loan(slice));
        n = z_slice_len(z_loan(slice));
    }
    /* A sample on a subscription is unsolicited by definition, so it gets the cov_ tag and
     * the envelope's seq — which for a notification is not a transaction id and pairs with
     * nothing. Tagging it says so, instead of printing a number that invites the reader to
     * look for a request that never existed. */
    uint64_t nseq = 0;
    if (d != NULL && n > 0) (void)qz_field_varint(d, n, 4, &nseq);
    (void)nseq;
    qz_log("COV", "[%u] %.*s  %zuB", st->count, (int)z_string_len(z_loan(ks)),
           z_string_data(z_loan(ks)), n);
    if (st->dump && d != NULL && n > 0) qz_packet_dump(d, n, true, "              ");
    if (d != NULL) z_drop(z_move(slice));
}

int qz_subscribe(qz_ctx_t *ctx, const char *keyexpr, unsigned seconds)
{
    if (!ctx->session_open) { qz_log("ERR", "no session — run `connect` first"); return -1; }
    sub_state_t st = {0, true};
    z_owned_closure_sample_t closure;
    z_closure_sample(&closure, on_any, NULL, &st);
    z_view_keyexpr_t ke;
    if (z_view_keyexpr_from_str(&ke, keyexpr) != Z_OK) {
        qz_log("ERR", "bad key expression: %s", keyexpr);
        return -1;
    }
    z_owned_subscriber_t sub;
    if (z_declare_subscriber(z_loan(ctx->session), &sub, z_loan(ke), z_move(closure), NULL)
        != Z_OK) {
        qz_log("ERR", "declare_subscriber failed");
        return -1;
    }
    qz_log("SUB", "%s for %us", keyexpr, seconds);
    sleep(seconds);
    z_drop(z_move(sub));
    qz_log("SUB", "%u sample(s)", st.count);
    return (int)st.count;
}

int qz_publish(qz_ctx_t *ctx, const char *keyexpr, const char *payload)
{
    if (!ctx->session_open) { qz_log("ERR", "no session — run `connect` first"); return -1; }
    z_view_keyexpr_t ke;
    if (z_view_keyexpr_from_str(&ke, keyexpr) != Z_OK) {
        qz_log("ERR", "bad key expression: %s", keyexpr);
        return -1;
    }
    z_owned_bytes_t bytes;
    z_bytes_copy_from_buf(&bytes, (const uint8_t *)payload, strlen(payload));
    z_put_options_t opts;
    z_put_options_default(&opts);
    opts.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
    if (z_put(z_loan(ctx->session), z_loan(ke), z_move(bytes), &opts) != Z_OK) {
        qz_log("ERR", "put failed");
        return -1;
    }
    qz_log("PUB", "%s  %zuB", keyexpr, strlen(payload));
    return 0;
}

/* ------------------------------------------------------------ request/reply */

typedef struct {
    uint32_t    seq;
    const char *service;      /* what this exchange is for; checked on the way back */
    char        key[QZ_MAX_KEY];   /* the key the reply actually arrived on */
    bool     got;
    uint64_t sent_ms;
    uint8_t  reply[2048];
    size_t   reply_len;
} rpc_state_t;

static void on_ack(z_loaned_sample_t *sample, void *arg)
{
    rpc_state_t *st = (rpc_state_t *)arg;
    z_owned_slice_t slice;
    if (z_bytes_to_slice(z_sample_payload(sample), &slice) != Z_OK) return;
    const uint8_t *d = z_slice_data(z_loan(slice));
    size_t n = z_slice_len(z_loan(slice));

    /* An ack is ours only if it echoes our seq AND names the service we asked. The key already
     * pins the board and our client_id, so those cannot cross; seq pins WHICH request, and
     * service_id catches the case seq alone cannot — a reply to a different service that
     * happens to carry the same number. Anything else is somebody else's answer and is left
     * alone rather than reported as this one's. */
    uint64_t seq = 0;
    if (!qz_field_varint(d, n, 4, &seq) || (uint32_t)seq != st->seq) { z_drop(z_move(slice)); return; }

    const uint8_t *svc = NULL;
    size_t svc_len = qz_field_bytes(d, n, 2, &svc);
    if (st->service != NULL && svc != NULL &&
        (svc_len != strlen(st->service) || memcmp(svc, st->service, svc_len) != 0)) {
        qz_log("RES", "ignored: seq %u came back as '%.*s', not %s",
               (unsigned)seq, (int)svc_len, (const char *)svc, st->service);
        z_drop(z_move(slice));
        return;
    }
    /* Record the key it came in on rather than the pattern we subscribed to: those differ,
     * and only one of them is what the board actually published. */
    z_view_string_t ks;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &ks);
    size_t klen = z_string_len(z_loan(ks));
    if (klen >= sizeof(st->key)) klen = sizeof(st->key) - 1;
    memcpy(st->key, z_string_data(z_loan(ks)), klen);
    st->key[klen] = '\0';

    if (n > sizeof(st->reply)) n = sizeof(st->reply);
    memcpy(st->reply, d, n);
    st->reply_len = n;
    st->got = true;
    z_drop(z_move(slice));
}

/* Shared body. `quiet` suppresses the per-packet dump and `reply_out` receives the reply's
 * payload — both exist so a command can read several services and render one view from them. */
static int qz_request_impl(qz_ctx_t *ctx, const char *service, qz_op_t op,
                           const uint8_t *payload, size_t payload_len, unsigned timeout_s,
                           bool quiet, uint8_t *reply_out, size_t reply_cap, size_t *reply_len)
{
    if (!ctx->session_open) { qz_log("ERR", "no session — run `connect` first"); return -1; }
    if (qz_session_ensure(ctx) != 0) return -1;
    if (ctx->board[0] == '\0') { qz_log("ERR", "no board selected — run `discover` or `use <id>`"); return -1; }

    /* seq is the contract's transaction id: the request carries it and the reply echoes it, and
     * it is the only thing that pairs the two. It used to be the low 16 bits of the millisecond
     * clock, which REPEATS EVERY 65.5 SECONDS — so two exchanges a minute apart, or two clients
     * started together, could carry the same number and each accept the other's reply.
     *
     * Now it is a per-session counter, +1 per call, started from a random 32-bit value so that
     * restarting this tool does not replay the numbers it used last time. */
    rpc_state_t st = {0};
    if (ctx->seq_next == 0) ctx->seq_next = (uint32_t)qz_now_ms() ^ (uint32_t)(uintptr_t)ctx;
    st.seq = ctx->seq_next++;
    st.service = service;

    /* Subscribe to exactly this call's reply. The board appends the transaction id to the ack
     * key, so zenoh does the matching and no other reply is ever delivered here — with several
     * calls in flight to one service the old shared key delivered every reply to every waiter,
     * which each then had to filter.
     *
     * The trailing `/**` costs nothing (it matches zero or more chunks) and leaves room for
     * the key to grow later. It does NOT make this tolerant of a board that publishes the bare
     * `.../ack/<client_id>`: the seq chunk is required, so such a board's reply never matches
     * and the wait times out — which the timeout message says outright rather than leaving as a
     * mystery. The seq and service_id checks in on_ack stay regardless: the key is an
     * optimisation, the envelope is the contract. */
    char ack_key[QZ_MAX_KEY], req_key[QZ_MAX_KEY];
    /* The verb and the transaction id lead, so a reply key is unique per call and this
     * subscription is exact — zenoh delivers this call's reply and nothing else. */
    snprintf(ack_key, sizeof(ack_key), "res/txn_%08x/%s/%s/svc/%s",
             st.seq, ctx->client_id, ctx->board, service);
    /* Same shape on the way out. The board subscribes on req, one wildcard where the seq goes. */
    snprintf(req_key, sizeof(req_key), "req/txn_%08x/%s/svc/%s", st.seq, ctx->board, service);

    /* Subscribe BEFORE publishing: the board answers in tens of milliseconds and a late
     * subscriber simply misses it. */
    z_owned_closure_sample_t closure;
    z_closure_sample(&closure, on_ack, NULL, &st);
    z_view_keyexpr_t ack_ke;
    z_view_keyexpr_from_str(&ack_ke, ack_key);
    z_owned_subscriber_t sub;
    if (z_declare_subscriber(z_loan(ctx->session), &sub, z_loan(ack_ke), z_move(closure), NULL)
        != Z_OK) {
        qz_log("ERR", "could not subscribe to %s", ack_key);
        return -1;
    }
    usleep(300 * 1000);

    uint8_t body[2048];
    int blen = qz_req_encode(body, sizeof(body), service, op, st.seq, ctx->client_id,
                             payload, payload_len);
    if (blen < 0) { z_drop(z_move(sub)); qz_log("ERR", "envelope too large"); return -1; }

    /* tx_ going out, rx_ coming back, cov_ for a notification nobody asked for. The NUMBER is
     * the same at both ends of one exchange — this tool's tx_41 is the board's rx_41 — so
     * grepping the number finds both halves; only the prefix says which half. */
    if (!quiet) {
        qz_log("REQ", "%s  %s  %dB", req_key, qz_op_name(op), blen);
        qz_packet_dump(body, (size_t)blen, false, "              ");
    }

    z_view_keyexpr_t req_ke;
    z_view_keyexpr_from_str(&req_ke, req_key);
    z_owned_bytes_t bytes;
    z_bytes_copy_from_buf(&bytes, body, (size_t)blen);
    z_put_options_t opts;
    z_put_options_default(&opts);
    opts.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
    st.sent_ms = qz_now_ms();
    if (z_put(z_loan(ctx->session), z_loan(req_ke), z_move(bytes), &opts) != Z_OK) {
        z_drop(z_move(sub));
        qz_log("ERR", "publishing the request failed");
        return -1;
    }

    for (unsigned i = 0; i < timeout_s * 20 && !st.got; i++) usleep(50 * 1000);
    z_drop(z_move(sub));

    if (!st.got) {
        /* Worth knowing: the board drops requests silently when its dispatch queue is full
         * (zero-timeout xQueueSend into a depth-8 queue), so a timeout does not necessarily
         * mean the request never arrived. */
        qz_log("REQ", "no reply within %us on %s", timeout_s, ack_key);
        {
            if (z_session_is_closed(z_loan(ctx->session)) ||
                _zp_send_keep_alive(_Z_RC_IN_VAL(z_loan(ctx->session))) != Z_OK) {
            qz_log("HINT", "the session DIED while waiting — the board likely rebooted. "
                           "Re-run the command (it reconnects), then `login` again for "
                           "authorized services.");
            return -1;
            }
        }
        qz_log("HINT", "that key carries the transaction id. A board built before the id was "
                       "added replies on ce/%s/svc/%s/res/%s with no trailing chunk, which "
                       "this subscription cannot match — flash it, or subscribe .../*/%s/**",
               ctx->board, service, ctx->client_id, ctx->client_id);
        return -1;
    }
    if (!quiet)
    qz_log("RES", "%s  %s  %zuB  round trip %llu ms",
           st.key[0] ? st.key : ack_key, qz_op_name(op), st.reply_len,
           (unsigned long long)(qz_now_ms() - st.sent_ms));
    if (!quiet) qz_packet_dump(st.reply, st.reply_len, true, "              ");

    if (reply_out != NULL && reply_len != NULL) {
        const uint8_t *pl = NULL;
        size_t n = qz_field_bytes(st.reply, st.reply_len, 6, &pl);   /* response payload */
        if (n > reply_cap) n = reply_cap;
        if (n > 0) memcpy(reply_out, pl, n);
        *reply_len = n;
    }

    /* Field 7 of a ResponseEnvelope is an ErrorInfo message, not a number. Printing its two
     * raw bytes said nothing; decoding it turns "<2 B> 08 04" into ERROR_PERMISSION. */
    const uint8_t *errbuf = NULL;
    size_t elen = qz_field_bytes(st.reply, st.reply_len, 7, &errbuf);
    uint64_t code = 0;
    if (elen > 0) (void)qz_field_varint(errbuf, elen, 1, &code);
    if (code != 0) {
        qz_log("RES", "FAILED — %s (code %llu)", qz_error_name(code), (unsigned long long)code);
        return -(int)code;
    }
    return 0;
}

int qz_request(qz_ctx_t *ctx, const char *service, qz_op_t op,
               const uint8_t *payload, size_t payload_len, unsigned timeout_s)
{
    return qz_request_impl(ctx, service, op, payload, payload_len, timeout_s,
                           false, NULL, 0, NULL);
}

int qz_request_quiet(qz_ctx_t *ctx, const char *service, qz_op_t op,
                     const uint8_t *payload, size_t payload_len, unsigned timeout_s,
                     uint8_t *reply_out, size_t reply_cap, size_t *reply_len)
{
    return qz_request_impl(ctx, service, op, payload, payload_len, timeout_s,
                           true, reply_out, reply_cap, reply_len);
}

/* --------------------------------------------------------------------- login */

int qz_login(qz_ctx_t *ctx, const char *password)
{
    if (ctx->board[0] == '\0') { qz_log("ERR", "no board selected"); return -1; }

    const char *nonce = NULL;
    for (size_t i = 0; i < ctx->board_count; i++)
        if (strcmp(ctx->boards[i].peer_id, ctx->board) == 0) nonce = ctx->boards[i].nonce;
    if (nonce == NULL || nonce[0] == '\0') {
        qz_log("ERR", "no nonce for %s — run `discover` first; the nonce is carried in the "
                      "announce beacon and changes on every board reboot", ctx->board);
        return -1;
    }

    /* proof = sha256( nonce_hex ":" client_id ":" hex(sha256(password)) )
     * The password itself never crosses the wire. Verified against ACB-M's
     * b_ZENOH_Verify_Proof, which hashes exactly this string. */
    uint8_t pw[32];
    char pwhex[65];
    qz_sha256_str(password, pw);
    qz_hex(pw, sizeof(pw), pwhex);

    char msg[QZ_NONCE_HEX + QZ_MAX_ID + 80];
    snprintf(msg, sizeof(msg), "%s:%s:%s", nonce, ctx->client_id, pwhex);
    uint8_t proof[32];
    qz_sha256_str(msg, proof);

    char proofhex[65];
    qz_hex(proof, sizeof(proof), proofhex);
    qz_log("AUTH", "nonce=%s client=%s", nonce, ctx->client_id);
    qz_log("AUTH", "proof=%s", proofhex);

    /* Report the verdict rather than describing how to read one. An earlier version printed
     * "a zero error code above means unlocked" whatever came back, so a refused login looked
     * like a successful one. */
    int rc = qz_request(ctx, "system.auth", QZ_OP_EXECUTE, proof, sizeof(proof), 10);
    if (rc == 0) {
        ctx->logged_in = true;
        qz_log("AUTH", "UNLOCKED as %s", ctx->client_id);
        return 0;
    }
    ctx->logged_in = false;
    if (rc == -4) {                       /* ERROR_PERMISSION */
        qz_log("AUTH", "DENIED. Either the password is wrong, or the nonce is stale — it "
                       "changes on every board reboot, so run `discover` again — or the "
                       "certificate CN does not match client_id '%s'.", ctx->client_id);
    } else {
        qz_log("AUTH", "no verdict from the board");
    }
    return -1;
}
