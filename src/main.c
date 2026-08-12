/** main.c — argument handling, the one-shot commands, and the interactive REPL. */
#include "qz.h"
#include "proto_tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_CERTS  "~/.config/acb-provisioner/certs/ce-acf23c0d8637"
#define DEFAULT_CA_URL "https://hackline.ca.nube-iiot.com"

static void expand_home(const char *in, char *out, size_t out_len)
{
    if (in[0] == '~' && in[1] == '/') {
        const char *home = getenv("HOME");
        snprintf(out, out_len, "%s%s", home ? home : "", in + 1);
    } else {
        snprintf(out, out_len, "%s", in);
    }
}

static void usage(void)
{
    printf(
"q_zenoh_testapp — a zenoh PEER that speaks the rubix contract, for testing ACB-M boards\n"
"\n"
"USAGE\n"
"  q_zenoh_testapp [options] [command [args]]\n"
"  q_zenoh_testapp [options]                # no command -> interactive REPL\n"
"\n"
"OPTIONS\n"
"  --certs DIR       directory with ca.pem, device.pem, device-key.pem\n"
"                    (default %s)\n"
"  --client-id ID    envelope client_id; defaults to the CN of device.pem, which is what\n"
"                    ADR-016 requires — the board refuses anything else\n"
"  --endpoint LOC    what to dial, e.g. tls/192.168.10.29:7447\n"
"  --board ID        board peer id to address, e.g. acbm-1cdbd4abbc7c\n"
"  --listen LOC      also listen, e.g. tls/0.0.0.0:7600 (lets others dial this peer)\n"
"  --verify-name     verify the peer certificate's SAN against the dialled address\n"
"                    (off by default: board certs pin their enrolment-day address)\n"
"  --ca-url URL      certificate authority (default %s)\n"
"  --ca-secret S     CA admin preshared secret, for `enroll`\n"
"  --debug, -d       show zenoh-pico's DEBUG/INFO lines (a line per frame and keep-alive).\n"
"                    Off by default; its WARN and ERROR lines are always shown either way.\n"
"                    QZ_DEBUG=1 does the same.\n"
"\n"
"COMMANDS\n"
"  scan [secs]             find boards by mDNS — no session and no address needed\n"
"  discover [secs]         listen for rubix/peers/*/announce; also reports each auth nonce\n"
"  use <board-id>          choose which board later commands address\n"
"  connect [endpoint]      open the mTLS peer session\n"
"  disconnect              close it\n"
"  login <password>        S1 login: sha256(nonce:client_id:sha256(password))\n"
"  logout                  system.auth with an empty payload\n"
"  sub <keyexpr> [secs]    subscribe and decode what arrives\n"
"  pub <keyexpr> <text>    publish a raw payload\n"
"  points <proto> read [ids...]      read point values (no ids = all)\n"
"  points <proto> write field=value  write one point, e.g. point_id=101 value=42\n"
"  points <proto> sub [secs]         subscribe to the COV notify stream\n"
"  config <proto> read               full config snapshot from the board\n"
"  config <proto> add-device field=value ...   upsert one device (a delta)\n"
"  config <proto> add-point  field=value ...   upsert one point  (a delta)\n"
"  config <proto> del-device|del-point <id>...\n"
"        <proto> is modbus, bacnet or lora. Field names and enum values are the\n"
"        contract's own; a wrong one lists what the .proto actually declares.\n"
"  req <service> [op]      protobuf request/reply; op = read|write|execute|ping|discover\n"
"  boardinfo               shorthand for `req system.boardinfo read`\n"
"  points                  shorthand for `req modbus.points read`\n"
"  enroll <id> [SAN]       get a certificate for <id> from the CA and write it to --certs\n"
"                          SAN example: IP:192.168.10.39  (needed if others will DIAL you)\n"
"  status                  what this session currently is\n"
"  help, quit\n"
"\n"
"EXAMPLES\n"
"  q_zenoh_testapp scan                                     # what is on the LAN?\n"
"  q_zenoh_testapp boardinfo                                # scans, connects, asks\n"
"  q_zenoh_testapp --endpoint tls/192.168.10.29:7447 discover 8\n"
"  q_zenoh_testapp --endpoint tls/192.168.10.29:7447 boardinfo\n"
"  q_zenoh_testapp --ca-secret $CA_ADMIN enroll q-test-01 IP:192.168.10.39\n"
"  q_zenoh_testapp --endpoint tls/192.168.10.29:7447        # then type commands\n",
        DEFAULT_CERTS, DEFAULT_CA_URL);
}

static qz_op_t parse_op(const char *s)
{
    if (s == NULL)                     return QZ_OP_READ;
    if (strcmp(s, "read") == 0)        return QZ_OP_READ;
    if (strcmp(s, "write") == 0)       return QZ_OP_WRITE;
    if (strcmp(s, "validate") == 0)    return QZ_OP_VALIDATE;
    if (strcmp(s, "subscribe") == 0)   return QZ_OP_SUBSCRIBE;
    if (strcmp(s, "execute") == 0)     return QZ_OP_EXECUTE;
    if (strcmp(s, "discover") == 0)    return QZ_OP_DISCOVER;
    if (strcmp(s, "ping") == 0)        return QZ_OP_PING;
    return QZ_OP_READ;
}

static int need_session(qz_ctx_t *ctx)
{
    if (ctx->session_open) return 0;
    if (ctx->endpoint[0] == '\0') {
        /* Rather than telling the operator to go and find an address, go and find it: the
         * board answers an mDNS browse, and its address comes from DHCP so a remembered one
         * goes stale anyway. */
        qz_log("SCAN", "no endpoint set — looking for boards on the LAN first");
        if (qz_mdns_scan(ctx, 4) <= 0 || ctx->endpoint[0] == '\0') {
            qz_log("ERR", "no board found. Pass --endpoint tls/<host>:7447, or run `scan` and "
                          "then `connect <address>`.");
            return -1;
        }
    }
    return qz_session_open(ctx);
}


/* The three field-bus services are the same shape with different leaf messages, so the
 * commands below are written once and the message is looked up per protocol. Names are built
 * fully qualified — PointWrite exists in all three packages, and a suffix match would pick
 * whichever sorted first. */
static const qz_pmsg_t *msg_for(const char *proto, const char *shape)
{
    char name[128], Proto[16];
    snprintf(Proto, sizeof Proto, "%s", proto);
    if (Proto[0] >= 'a' && Proto[0] <= 'z') Proto[0] = (char)(Proto[0] - 32);
    if (strstr(shape, "%s") != NULL) {
        char leaf[64];
        snprintf(leaf, sizeof leaf, shape, Proto);
        snprintf(name, sizeof name, "rubix.embedded.%s.v1.%s", proto, leaf);
    } else {
        snprintf(name, sizeof name, "rubix.embedded.%s.v1.%s", proto, shape);
    }
    const qz_pmsg_t *m = qz_msg_find(name);
    if (m == NULL) qz_log("ERR", "the contract has no %s", name);
    return m;
}

static bool known_proto(const char *p)
{
    if (strcmp(p, "modbus") == 0 || strcmp(p, "bacnet") == 0 || strcmp(p, "lora") == 0) return true;
    qz_log("ERR", "protocol must be modbus, bacnet or lora (got '%s')", p);
    return false;
}

int qz_run_command(qz_ctx_t *ctx, int argc, char **argv)
{
    if (argc < 1) return -1;
    const char *cmd = argv[0];

    if (strcmp(cmd, "help") == 0)  { usage(); return 0; }
    if (strcmp(cmd, "status") == 0) {
        printf("  endpoint  %s\n  certs     %s\n  client_id %s\n  board     %s\n"
               "  session   %s\n  logged in %s\n  boards    %zu seen\n",
               ctx->endpoint[0] ? ctx->endpoint : "(unset)", ctx->certs_dir,
               ctx->client_id[0] ? ctx->client_id : "(from cert CN)",
               ctx->board[0] ? ctx->board : "(unset)",
               ctx->session_open ? "open" : "closed",
               ctx->logged_in ? "yes" : "no", ctx->board_count);
        return 0;
    }
    if (strcmp(cmd, "connect") == 0) {
        if (argc > 1) snprintf(ctx->endpoint, sizeof(ctx->endpoint), "%s", argv[1]);
        return qz_session_open(ctx);
    }
    if (strcmp(cmd, "disconnect") == 0) { qz_session_close(ctx); return 0; }
    if (strcmp(cmd, "use") == 0) {
        if (argc < 2) { qz_log("ERR", "use <board-id>"); return -1; }
        snprintf(ctx->board, sizeof(ctx->board), "%s", argv[1]);
        qz_log("SELECT", "commands now address %s", ctx->board);
        return 0;
    }
    if (strcmp(cmd, "scan") == 0) {
        return qz_mdns_scan(ctx, argc > 1 ? (unsigned)atoi(argv[1]) : 4) >= 0 ? 0 : -1;
    }
    if (strcmp(cmd, "discover") == 0) {
        if (need_session(ctx) != 0) return -1;
        return qz_discover(ctx, argc > 1 ? (unsigned)atoi(argv[1]) : 6) >= 0 ? 0 : -1;
    }
    if (strcmp(cmd, "sub") == 0) {
        if (argc < 2) { qz_log("ERR", "sub <keyexpr> [seconds]"); return -1; }
        if (need_session(ctx) != 0) return -1;
        return qz_subscribe(ctx, argv[1], argc > 2 ? (unsigned)atoi(argv[2]) : 10) >= 0 ? 0 : -1;
    }
    if (strcmp(cmd, "pub") == 0) {
        if (argc < 3) { qz_log("ERR", "pub <keyexpr> <text>"); return -1; }
        if (need_session(ctx) != 0) return -1;
        return qz_publish(ctx, argv[1], argv[2]);
    }
    if (strcmp(cmd, "login") == 0) {
        if (argc < 2) { qz_log("ERR", "login <password>"); return -1; }
        if (need_session(ctx) != 0) return -1;
        if (ctx->board_count == 0) qz_discover(ctx, 4);   /* the nonce lives in the announce */
        return qz_login(ctx, argv[1]);
    }
    if (strcmp(cmd, "logout") == 0) {
        if (need_session(ctx) != 0) return -1;
        return qz_request(ctx, "system.auth", QZ_OP_EXECUTE, NULL, 0, 10);
    }
    /* points <proto> read|write|sub — the data plane */
    if (strcmp(cmd, "points") == 0 && argc >= 3) {
        if (!known_proto(argv[1])) return -1;
        char service[32];
        snprintf(service, sizeof service, "%s.points", argv[1]);
        if (need_session(ctx) != 0) return -1;
        if (ctx->board[0] == '\0') qz_discover(ctx, 4);

        if (strcmp(argv[2], "read") == 0) {
            uint8_t payload[256];
            size_t plen = 0;
            if (argc > 3) {                       /* an explicit id list */
                uint32_t ids[32];
                size_t n = 0;
                for (int i = 3; i < argc && n < 32; i++) ids[n++] = (uint32_t)strtoul(argv[i], NULL, 0);
                plen = qz_packed_u32(payload, sizeof payload, 1, ids, n);
            } else {
                /* PointReadRequest.all — an empty payload would read as "no ids and not all",
                 * which is a different request from "everything". */
                char *all[] = { (char *)"all=true" };
                if (qz_build(msg_for(argv[1], "PointReadRequest"), 1, all,
                             payload, sizeof payload, &plen) != 0) return -1;
            }
            return qz_request(ctx, service, QZ_OP_READ, payload, plen, 10);
        }
        if (strcmp(argv[2], "write") == 0) {
            uint8_t payload[256];
            size_t plen = 0;
            if (qz_build(msg_for(argv[1], "PointWrite"), argc - 3, argv + 3,
                         payload, sizeof payload, &plen) != 0) return -1;
            return qz_request(ctx, service, QZ_OP_WRITE, payload, plen, 10);
        }
        if (strcmp(argv[2], "sub") == 0) {
            char key[192];
            /* `/**` matches zero or more chunks, so this catches a notify key stamped with
             * the board's device time as well as a bare one. */
            snprintf(key, sizeof key, "rubix/%s/svc/%s/notify/**", ctx->board, service);
            return qz_subscribe(ctx, key, argc > 3 ? (unsigned)atoi(argv[3]) : 30) >= 0 ? 0 : -1;
        }
        qz_log("ERR", "points <proto> read|write|sub");
        return -1;
    }

    /* config <proto> read|add-device|add-point|del-device|del-point — the control plane.
     * Every write is a DELTA: the contract has no replaceAll, so nothing here can clear a
     * board by omission. */
    if (strcmp(cmd, "config") == 0 && argc >= 3) {
        if (!known_proto(argv[1])) return -1;
        char service[32];
        snprintf(service, sizeof service, "%s.config", argv[1]);
        if (need_session(ctx) != 0) return -1;
        if (ctx->board[0] == '\0') qz_discover(ctx, 4);

        if (strcmp(argv[2], "read") == 0)
            return qz_request(ctx, service, QZ_OP_READ, NULL, 0, 10);

        uint8_t leaf[384], delta[512];
        size_t llen = 0, dlen = 0;
        if (strcmp(argv[2], "add-device") == 0) {
            if (qz_build(msg_for(argv[1], "%sDeviceDef"), argc - 3, argv + 3,
                         leaf, sizeof leaf, &llen) != 0) return -1;
            dlen = qz_wrap(delta, sizeof delta, 1, leaf, llen);       /* upsert_devices */
        } else if (strcmp(argv[2], "add-point") == 0) {
            if (qz_build(msg_for(argv[1], "%sPointDef"), argc - 3, argv + 3,
                         leaf, sizeof leaf, &llen) != 0) return -1;
            dlen = qz_wrap(delta, sizeof delta, 2, leaf, llen);       /* upsert_points */
        } else if (strcmp(argv[2], "del-device") == 0 || strcmp(argv[2], "del-point") == 0) {
            if (argc < 4) { qz_log("ERR", "config <proto> %s <id>", argv[2]); return -1; }
            uint32_t ids[16];
            size_t n = 0;
            for (int i = 3; i < argc && n < 16; i++) ids[n++] = (uint32_t)strtoul(argv[i], NULL, 0);
            dlen = qz_packed_u32(delta, sizeof delta,
                                 strcmp(argv[2], "del-device") == 0 ? 3 : 4, ids, n);
        } else {
            qz_log("ERR", "config <proto> read|add-device|add-point|del-device|del-point");
            return -1;
        }
        return qz_request(ctx, service, QZ_OP_WRITE, delta, dlen, 10);
    }

    if (strcmp(cmd, "req") == 0) {
        if (argc < 2) { qz_log("ERR", "req <service> [op]"); return -1; }
        if (need_session(ctx) != 0) return -1;
        if (ctx->board[0] == '\0') qz_discover(ctx, 4);
        return qz_request(ctx, argv[1], parse_op(argc > 2 ? argv[2] : NULL), NULL, 0, 10);
    }
    if (strcmp(cmd, "boardinfo") == 0) {
        if (need_session(ctx) != 0) return -1;
        if (ctx->board[0] == '\0') qz_discover(ctx, 4);
        return qz_request(ctx, "system.boardinfo", QZ_OP_READ, NULL, 0, 10);
    }
    if (strcmp(cmd, "points") == 0) {
        if (need_session(ctx) != 0) return -1;
        if (ctx->board[0] == '\0') qz_discover(ctx, 4);
        return qz_request(ctx, "modbus.points", QZ_OP_READ, NULL, 0, 10);
    }
    if (strcmp(cmd, "enroll") == 0) {
        if (argc < 2) { qz_log("ERR", "enroll <id> [SAN]"); return -1; }
        if (ctx->ca_admin_secret[0] == '\0') {
            const char *env = getenv("CA_ADMIN_SECRET");
            if (env != NULL) snprintf(ctx->ca_admin_secret, sizeof(ctx->ca_admin_secret), "%s", env);
        }
        if (ctx->ca_admin_secret[0] == '\0') {
            qz_log("ERR", "no CA admin secret — pass --ca-secret or set CA_ADMIN_SECRET");
            return -1;
        }
        return qz_ca_enroll(ctx->ca_url, ctx->ca_admin_secret, argv[1],
                            argc > 2 ? argv[2] : NULL, ctx->certs_dir);
    }
    qz_log("ERR", "unknown command '%s' — try `help`", cmd);
    return -1;
}

int qz_repl(qz_ctx_t *ctx)
{
    printf("q_zenoh_testapp — type `help` for commands, `quit` to leave\n");
    char line[1024];
    for (;;) {
        printf("q> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) break;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;

        /* Split on spaces, but keep the tail of `pub` intact so a payload may contain them. */
        /* Room for a full `config <proto> add-point` line: nine field=value pairs plus the
         * three leading words. It used to stop at eight tokens and drop the rest in silence,
         * which sent a device definition missing its parity and reported success. */
        char *argv[32];
        int argc = 0;
        char *p = line;
        while (*p != '\0') {
            while (*p == ' ') p++;
            if (*p == '\0') break;
            if (argc == (int)(sizeof argv / sizeof argv[0])) {
                qz_log("ERR", "too many arguments (max %zu) — nothing was sent",
                       sizeof argv / sizeof argv[0]);
                argc = 0;
                break;
            }
            if (argc == 2 && strncmp(line, "pub ", 4) == 0) { argv[argc++] = p; break; }
            argv[argc++] = p;
            while (*p != '\0' && *p != ' ') p++;
            if (*p == ' ') *p++ = '\0';
        }
        if (argc > 0) qz_run_command(ctx, argc, argv);
    }
    qz_session_close(ctx);
    return 0;
}

int main(int argc, char **argv)
{
    qz_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    expand_home(DEFAULT_CERTS, ctx.certs_dir, sizeof(ctx.certs_dir));
    snprintf(ctx.ca_url, sizeof(ctx.ca_url), "%s", DEFAULT_CA_URL);
    ctx.verify_name = false;

    bool verbose = (getenv("QZ_DEBUG") != NULL && getenv("QZ_DEBUG")[0] == '1');

    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0) verbose = true;
    }
    /* Before anything can print: this replaces stdout with a filtered pipe. */
    qz_log_filter_install(verbose);

    i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--certs") == 0 && i + 1 < argc)
            expand_home(argv[++i], ctx.certs_dir, sizeof(ctx.certs_dir));
        else if (strcmp(argv[i], "--client-id") == 0 && i + 1 < argc)
            snprintf(ctx.client_id, sizeof(ctx.client_id), "%s", argv[++i]);
        else if (strcmp(argv[i], "--endpoint") == 0 && i + 1 < argc)
            snprintf(ctx.endpoint, sizeof(ctx.endpoint), "%s", argv[++i]);
        else if (strcmp(argv[i], "--board") == 0 && i + 1 < argc)
            snprintf(ctx.board, sizeof(ctx.board), "%s", argv[++i]);
        else if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc)
            snprintf(ctx.listen, sizeof(ctx.listen), "%s", argv[++i]);
        else if (strcmp(argv[i], "--ca-url") == 0 && i + 1 < argc)
            snprintf(ctx.ca_url, sizeof(ctx.ca_url), "%s", argv[++i]);
        else if (strcmp(argv[i], "--ca-secret") == 0 && i + 1 < argc)
            snprintf(ctx.ca_admin_secret, sizeof(ctx.ca_admin_secret), "%s", argv[++i]);
        else if (strcmp(argv[i], "--verify-name") == 0) ctx.verify_name = true;
        else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0) { /* handled */ }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        else break;
    }

    int rc;
    if (i < argc) {
        rc = qz_run_command(&ctx, argc - i, &argv[i]);
        qz_session_close(&ctx);
    } else {
        rc = qz_repl(&ctx);
    }
    return rc == 0 ? 0 : 1;
}
