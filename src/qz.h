/**
 * qz.h — shared context and small helpers for q_zenoh_testapp.
 *
 * The app is a real zenoh PEER, not a client: it opens the same kind of session an ACB-M
 * board opens, over the same mutual TLS, so what it proves about the board is what a real
 * consumer would experience.
 */
#ifndef QZ_H
#define QZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zenoh-pico.h>

#define QZ_MAX_ID       64
#define QZ_MAX_PATH     512
#define QZ_MAX_KEY      256
#define QZ_MAX_BOARDS   16
#define QZ_NONCE_HEX    33          /* 16 random bytes as hex + NUL, as the board emits */

/** One board seen on the bus, learned from its announce beacon. */
typedef struct {
    char     peer_id[QZ_MAX_ID];
    char     nonce[QZ_NONCE_HEX];   /* per-boot, carried in the announce payload */
    char     addr[64];              /* from mDNS: "tls/<ip>:<port>", "" if unknown */
    uint32_t announces;
    uint64_t last_seen_ms;
} qz_board_t;

/** Everything a command needs. One session at a time — `connect` replaces it. */
typedef struct {
    /* identity + transport */
    char  certs_dir[QZ_MAX_PATH];   /* holds ca.pem, device.pem, device-key.pem */
    char  client_id[QZ_MAX_ID];     /* MUST equal the CN in device.pem — see ADR-016 */
    char  endpoint[QZ_MAX_PATH];    /* tls/<host>:<port> we dial */
    char  listen[QZ_MAX_PATH];      /* optional own listen locator, "" for none */
    bool  verify_name;              /* verify the peer's SAN against the address we dial */

    /* target */
    char  board[QZ_MAX_ID];         /* peer id of the board commands address */

    /* live session */
    z_owned_session_t session;
    bool              session_open;
    bool              logged_in;
    /* The contract's transaction id, handed out +1 per request. Random start so a restart does
     * not replay the numbers the previous run used. */
    uint32_t          seq_next;

    /* discovery */
    qz_board_t boards[QZ_MAX_BOARDS];
    size_t     board_count;

    /* CA, for `enroll` */
    char ca_url[QZ_MAX_PATH];
    char ca_admin_secret[128];
} qz_ctx_t;

/* ----------------------------------------------------------- logfilter.c */

/** Hides zenoh-pico's DEBUG/INFO chatter unless `verbose`. WARN and ERROR always pass. */
void qz_log_filter_install(bool verbose);

/* ---------------------------------------------------------------- util.c */

void     qz_log(const char *tag, const char *fmt, ...);
void     qz_hex(const uint8_t *in, size_t len, char *out);   /* out: 2*len+1 */
void     qz_sha256(const uint8_t *in, size_t len, uint8_t out[32]);
void     qz_sha256_str(const char *s, uint8_t out[32]);
int      qz_hmac_sha256_b64(const char *key, const char *msg, char *out, size_t out_len);
int      qz_b64(const uint8_t *in, size_t in_len, char *out, size_t out_len);
uint64_t qz_now_ms(void);
int      qz_read_file(const char *path, char *buf, size_t buf_len);
/** CN of an X.509 certificate in PEM form. Returns 0 on success. */
int      qz_cert_cn(const char *pem_path, char *out, size_t out_len);

/* --------------------------------------------------------------- proto.c */

/** Envelope field numbers verified against ACB-M's generated envelope.pb.h. */
typedef enum {
    QZ_OP_UNKNOWN = 0, QZ_OP_READ = 1, QZ_OP_WRITE = 2, QZ_OP_VALIDATE = 3,
    QZ_OP_SUBSCRIBE = 4, QZ_OP_EXECUTE = 5, QZ_OP_DISCOVER = 6, QZ_OP_PING = 7,
    QZ_OP_RESET = 8,          /* the contract's last operation; was missing here */
} qz_op_t;

/** Builds a RequestEnvelope. Returns the number of bytes written, or -1. */
int qz_req_encode(uint8_t *buf, size_t buf_len, const char *service_id, qz_op_t op,
                  uint32_t seq, const char *client_id,
                  const uint8_t *payload, size_t payload_len);

/** Prints every top-level field of an envelope, naming the ones we know. */

/** Renders a protobuf message as readable JSON. Field names are known for a few payload
 *  types; the rest keep their field number, so the output never invents a label. */

/** Prints a packet twice: the bytes exactly as sent, then the same thing as JSON. */
void qz_packet_dump(const uint8_t *buf, size_t len, bool is_response, const char *indent);

/** Renders an ErrorInfo and names its code. */
const char *qz_error_name(uint64_t code);

/** "READ", "WRITE", … for log lines that have to be readable when calls interleave. */
const char *qz_op_name(qz_op_t op);

/** Copies out a length-delimited field. Returns its length, or 0 when absent. */
/* build.c — payloads assembled from `field=value` using the generated contract schema, so
 * field names and enum labels are the .proto's own and never a second copy of them. */
struct qz_pmsg;
int    qz_build(const struct qz_pmsg *msg, int argc, char **argv, uint8_t *out, size_t cap,
                size_t *len);
size_t qz_wrap(uint8_t *out, size_t cap, uint32_t field, const uint8_t *sub, size_t sublen);
size_t qz_packed_u32(uint8_t *out, size_t cap, uint32_t field, const uint32_t *ids, size_t n);

size_t qz_field_bytes(const uint8_t *buf, size_t len, uint32_t field,
                      const uint8_t **out);

/** Pulls one varint field out of an envelope. Returns false when absent. */
bool qz_field_varint(const uint8_t *buf, size_t len, uint32_t field, uint64_t *out);

/* ---------------------------------------------------------------- ca.c */

/** Enrols `id` with the CA: register, fetch root, generate an RSA-2048 key and CSR, sign,
 *  and write ca.pem / device.pem / device-key.pem / device.secret into `out_dir`.
 *  `san` may be NULL; when set (e.g. "IP:192.168.10.39") it is put in the CSR, which the
 *  hackline CA preserves — needed when a peer will be DIALLED at that address. */
int qz_ca_enroll(const char *ca_url, const char *admin_secret, const char *id,
                 const char *san, const char *out_dir);

/* -------------------------------------------------------------- session.c */

/** Finds boards on the LAN with an mDNS query for _zenoh._tcp — needs no session and no
 *  endpoint, which is the point: it is how you learn an address you do not know yet. */
int  qz_mdns_scan(qz_ctx_t *ctx, unsigned seconds);

int  qz_session_open(qz_ctx_t *ctx);
/* Reconnects a session the board's reboot killed; 0 = usable. */
int  qz_session_ensure(qz_ctx_t *ctx);
void qz_session_close(qz_ctx_t *ctx);
int  qz_discover(qz_ctx_t *ctx, unsigned seconds);
int  qz_subscribe(qz_ctx_t *ctx, const char *keyexpr, unsigned seconds);
int  qz_publish(qz_ctx_t *ctx, const char *keyexpr, const char *payload);
int  qz_login(qz_ctx_t *ctx, const char *password);
int  qz_request(qz_ctx_t *ctx, const char *service, qz_op_t op,
                const uint8_t *payload, size_t payload_len, unsigned timeout_s);

/** As qz_request, but hands the reply's PAYLOAD back and can keep the packet dump quiet.
 *  A command that reads three services to render one view wants the data, not three dumps. */
int  qz_request_quiet(qz_ctx_t *ctx, const char *service, qz_op_t op,
                      const uint8_t *payload, size_t payload_len, unsigned timeout_s,
                      uint8_t *reply_out, size_t reply_cap, size_t *reply_len);

/* ----------------------------------------------------------- flow.c */

/** Uploads a .riot application over system.flow (begin/data/commit) and prints the verdict.
 *  `corrupt` flips one byte of chunk 0 after the sha was taken — the acceptance test that a
 *  bad upload leaves the old flow running. */


/** Prints FlowStatus — what the board is running, its sha, the sticky error. */

/* ----------------------------------------------------------- inventory.c */

/** Prints every device and its points, across all three config planes.
 *  The board is the only authority on this — its config lives in RAM, so after any restart
 *  what it holds is the only truth about what it holds. */
int qz_inventory(qz_ctx_t *ctx);

/* ---------------------------------------------------------------- repl.c */

int qz_repl(qz_ctx_t *ctx);
int qz_run_command(qz_ctx_t *ctx, int argc, char **argv);

#endif /* QZ_H */
