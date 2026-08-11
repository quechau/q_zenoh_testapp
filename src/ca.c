/** ca.c — enrolment against the hackline CA, entirely on Mbed TLS.
 *
 * Mirrors what the ACB Provisioner does for a host identity, minus the serial steps that only
 * apply to a board: device secret, register (admin HMAC), fetch the root, RSA-2048 key + CSR
 * with CN = the id, sign (device HMAC), verify.
 *
 * Auth scheme, from the CA's own client:
 *   X-GlobalUUID: <uuid>       ("admin" for admin endpoints)
 *   X-Timestamp:  <unix seconds>
 *   X-HMAC:       base64( HMAC-SHA256( key=secret, msg = uuid + timestamp + secret ) )
 */
#include "qz.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_csr.h>

#define HTTP_BUF 65536
#define SYSTEM_CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"

typedef struct {
    char host[256];
    char port[8];
    char path_prefix[256];
} url_t;

static int split_url(const char *url, url_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) { p += 8; snprintf(out->port, sizeof(out->port), "443"); }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; snprintf(out->port, sizeof(out->port), "80"); }
    else return -1;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    if (colon != NULL && (slash == NULL || colon < slash)) {
        hl = (size_t)(colon - p);
        size_t pl = (slash ? (size_t)(slash - colon) : strlen(colon)) - 1;
        if (pl >= sizeof(out->port)) return -1;
        memcpy(out->port, colon + 1, pl);
        out->port[pl] = '\0';
    }
    if (hl >= sizeof(out->host)) return -1;
    memcpy(out->host, p, hl);
    out->host[hl] = '\0';
    if (slash != NULL) snprintf(out->path_prefix, sizeof(out->path_prefix), "%s", slash);
    return 0;
}

/** One HTTPS request. Returns the status code, or -1; body (NUL-terminated) goes to `body`. */
static int https_request(const url_t *u, const char *method, const char *path,
                         const char *headers, const char *req_body,
                         char *body, size_t body_len)
{
    int ret = -1, status = -1;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;

    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    const char *pers = "q_zenoh_testapp";
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) goto done;

    /* Verify the CA server with the system trust store when it exists. An admin secret is
     * about to be sent, so an unverified connection is called out rather than done quietly. */
    int have_trust = (mbedtls_x509_crt_parse_file(&cacert, SYSTEM_CA_BUNDLE) == 0);
    if (!have_trust)
        qz_log("WARN", "no system CA bundle at %s — the CA server's certificate will not be "
                       "verified", SYSTEM_CA_BUNDLE);

    if (mbedtls_net_connect(&net, u->host, u->port, MBEDTLS_NET_PROTO_TCP) != 0) {
        qz_log("ERR", "cannot reach %s:%s", u->host, u->port);
        goto done;
    }
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto done;
    mbedtls_ssl_conf_authmode(&conf, have_trust ? MBEDTLS_SSL_VERIFY_REQUIRED
                                                : MBEDTLS_SSL_VERIFY_NONE);
    if (have_trust) mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    if (mbedtls_ssl_setup(&ssl, &conf) != 0) goto done;
    if (mbedtls_ssl_set_hostname(&ssl, u->host) != 0) goto done;
    mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            qz_log("ERR", "TLS handshake with %s failed: -0x%04x", u->host, -ret);
            goto done;
        }
    }

    char req[8192];
    int n = snprintf(req, sizeof(req),
                     "%s %s%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                     "User-Agent: q_zenoh_testapp\r\n%s"
                     "Content-Length: %zu\r\n\r\n%s",
                     method, u->path_prefix, path, u->host,
                     headers ? headers : "", req_body ? strlen(req_body) : 0,
                     req_body ? req_body : "");
    if (n < 0 || (size_t)n >= sizeof(req)) { qz_log("ERR", "request too large"); goto done; }

    size_t written = 0;
    while (written < (size_t)n) {
        ret = mbedtls_ssl_write(&ssl, (const unsigned char *)req + written, (size_t)n - written);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret <= 0) goto done;
        written += (size_t)ret;
    }

    static char raw[HTTP_BUF];
    size_t got = 0;
    for (;;) {
        ret = mbedtls_ssl_read(&ssl, (unsigned char *)raw + got, sizeof(raw) - 1 - got);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) break;
        if (ret < 0) break;
        got += (size_t)ret;
        if (got >= sizeof(raw) - 1) break;
    }
    raw[got] = '\0';

    if (sscanf(raw, "HTTP/1.%*d %d", &status) != 1) { status = -1; goto done; }
    const char *sep = strstr(raw, "\r\n\r\n");
    const char *payload = sep ? sep + 4 : raw;
    snprintf(body, body_len, "%s", payload);

done:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&net);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return status;
}

static int hmac_headers(const char *uuid, const char *secret, char *out, size_t out_len)
{
    char ts[24];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));
    char msg[512];
    snprintf(msg, sizeof(msg), "%s%s%s", uuid, ts, secret);
    char mac[128];
    if (qz_hmac_sha256_b64(secret, msg, mac, sizeof(mac)) != 0) return -1;
    snprintf(out, out_len,
             "X-GlobalUUID: %s\r\nX-Timestamp: %s\r\nX-HMAC: %s\r\n"
             "Content-Type: application/json\r\n", uuid, ts, mac);
    return 0;
}

static int write_file(const char *path, const char *data, mode_t mode)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) { qz_log("ERR", "cannot write %s: %s", path, strerror(errno)); return -1; }
    fwrite(data, 1, strlen(data), f);
    fclose(f);
    chmod(path, mode);
    return 0;
}

/** Extracts a JSON string value. Enough for these small, predictable replies. */
static int json_str(const char *json, const char *key, char *out, size_t out_len)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (p == NULL) return -1;
    p = strchr(p + strlen(pat), ':');
    if (p == NULL) return -1;
    while (*p == ':' || *p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p != '\0' && *p != '"' && i + 1 < out_len) {
        if (*p == '\\' && p[1] == 'n') { out[i++] = '\n'; p += 2; continue; }
        if (*p == '\\' && p[1] == '"') { out[i++] = '"';  p += 2; continue; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int add_san(mbedtls_x509write_csr *csr, const char *san)
{
    /* "IP:1.2.3.4" or "DNS:name". The hackline CA copies SANs from the CSR into the issued
     * certificate, which is what makes a peer dialable by address with name verification on. */
    mbedtls_x509_san_list node;
    memset(&node, 0, sizeof(node));
    unsigned char ip[4];
    if (strncmp(san, "IP:", 3) == 0) {
        unsigned a, b, c, d;
        if (sscanf(san + 3, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
        ip[0] = (unsigned char)a; ip[1] = (unsigned char)b;
        ip[2] = (unsigned char)c; ip[3] = (unsigned char)d;
        node.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
        node.node.san.unstructured_name.p = ip;
        node.node.san.unstructured_name.len = sizeof(ip);
    } else if (strncmp(san, "DNS:", 4) == 0) {
        node.node.type = MBEDTLS_X509_SAN_DNS_NAME;
        node.node.san.unstructured_name.p = (unsigned char *)(uintptr_t)(san + 4);
        node.node.san.unstructured_name.len = strlen(san + 4);
    } else {
        return -1;
    }
    node.next = NULL;
    return mbedtls_x509write_csr_set_subject_alternative_name(csr, &node);
}

int qz_ca_enroll(const char *ca_url, const char *admin_secret, const char *id,
                 const char *san, const char *out_dir)
{
    url_t u;
    if (split_url(ca_url, &u) != 0) { qz_log("ERR", "bad CA url: %s", ca_url); return -1; }
    mkdir(out_dir, 0755);

    char ca_path[QZ_MAX_PATH], cert_path[QZ_MAX_PATH], key_path[QZ_MAX_PATH];
    char csr_path[QZ_MAX_PATH], sec_path[QZ_MAX_PATH];
    snprintf(ca_path,   sizeof(ca_path),   "%s/ca.pem",         out_dir);
    snprintf(cert_path, sizeof(cert_path), "%s/device.pem",     out_dir);
    snprintf(key_path,  sizeof(key_path),  "%s/device-key.pem", out_dir);
    snprintf(csr_path,  sizeof(csr_path),  "%s/device.csr",     out_dir);
    snprintf(sec_path,  sizeof(sec_path),  "%s/device.secret",  out_dir);

    static char body[HTTP_BUF];
    static char reqbody[8192];
    char headers[512];

    /* 1. device secret — generated once and reused, so re-running is idempotent. */
    char secret[96] = {0};
    if (qz_read_file(sec_path, secret, sizeof(secret)) > 0) {
        secret[strcspn(secret, "\r\n")] = '\0';
        qz_log("STEP", "secret    reused from %s", sec_path);
    } else {
        mbedtls_entropy_context e; mbedtls_ctr_drbg_context g;
        mbedtls_entropy_init(&e); mbedtls_ctr_drbg_init(&g);
        const char *pers = "qz-secret";
        uint8_t raw[24];
        if (mbedtls_ctr_drbg_seed(&g, mbedtls_entropy_func, &e,
                                  (const unsigned char *)pers, strlen(pers)) != 0 ||
            mbedtls_ctr_drbg_random(&g, raw, sizeof(raw)) != 0) {
            mbedtls_ctr_drbg_free(&g); mbedtls_entropy_free(&e);
            qz_log("ERR", "could not generate a device secret"); return -1;
        }
        mbedtls_ctr_drbg_free(&g); mbedtls_entropy_free(&e);
        qz_b64(raw, sizeof(raw), secret, sizeof(secret));
        char withnl[128];
        snprintf(withnl, sizeof(withnl), "%s\n", secret);
        write_file(sec_path, withnl, 0600);
        qz_log("STEP", "secret    generated");
    }

    /* 2. register (admin HMAC). 409 means it already exists — reuse the stored secret. */
    if (hmac_headers("admin", admin_secret, headers, sizeof(headers)) != 0) return -1;
    snprintf(reqbody, sizeof(reqbody),
             "{\"global_uuid\":\"%s\",\"preshared_secret\":\"%s\"}", id, secret);
    int st = https_request(&u, "POST", "/devices/register", headers, reqbody, body, sizeof(body));
    if (st == 200 || st == 201) qz_log("STEP", "register  created %s", id);
    else if (st == 409)         qz_log("STEP", "register  already exists — reusing the stored secret");
    else { qz_log("ERR", "register -> HTTP %d: %.200s", st, body); return -1; }

    /* 3. CA root */
    st = https_request(&u, "GET", "/ca/certificate", NULL, NULL, body, sizeof(body));
    if (st != 200 || strstr(body, "BEGIN CERTIFICATE") == NULL) {
        qz_log("ERR", "GET /ca/certificate -> HTTP %d", st); return -1;
    }
    if (write_file(ca_path, body, 0644) != 0) return -1;
    qz_log("STEP", "ca-cert   %s", ca_path);

    /* 4. RSA-2048 key. The CA refuses ECDSA CSRs, and 4096 is needlessly slow for a TLS
     *    accept on the devices this talks to. */
    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    const char *pers = "qz-key";
    static unsigned char keybuf[4096];
    int rc = -1;
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) goto key_done;
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) goto key_done;
    qz_log("STEP", "keypair   generating RSA-2048 …");
    if (mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk), mbedtls_ctr_drbg_random, &drbg, 2048, 65537) != 0)
        goto key_done;
    if (mbedtls_pk_write_key_pem(&pk, keybuf, sizeof(keybuf)) != 0) goto key_done;
    if (write_file(key_path, (const char *)keybuf, 0600) != 0) goto key_done;
    rc = 0;
key_done:
    if (rc != 0) {
        qz_log("ERR", "key generation failed");
        mbedtls_pk_free(&pk); mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&entropy);
        return -1;
    }

    /* 5. CSR, CN = the id (ADR-016 binds the envelope client_id to this CN). */
    mbedtls_x509write_csr csr;
    mbedtls_x509write_csr_init(&csr);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&csr, &pk);
    char subject[128];
    snprintf(subject, sizeof(subject), "CN=%s", id);
    static unsigned char csrbuf[8192];
    rc = -1;
    if (mbedtls_x509write_csr_set_subject_name(&csr, subject) != 0) goto csr_done;
    if (san != NULL && add_san(&csr, san) != 0) {
        qz_log("ERR", "bad SAN '%s' — expected IP:a.b.c.d or DNS:name", san);
        goto csr_done;
    }
    if (mbedtls_x509write_csr_pem(&csr, csrbuf, sizeof(csrbuf),
                                  mbedtls_ctr_drbg_random, &drbg) != 0) goto csr_done;
    write_file(csr_path, (const char *)csrbuf, 0644);
    rc = 0;
csr_done:
    mbedtls_x509write_csr_free(&csr);
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    if (rc != 0) { qz_log("ERR", "CSR generation failed"); return -1; }
    qz_log("STEP", "csr       CN=%s%s%s", id, san ? "  SAN=" : "", san ? san : "");

    /* 6. sign (device HMAC) */
    if (hmac_headers(id, secret, headers, sizeof(headers)) != 0) return -1;
    /* the CSR PEM must be embedded as a JSON string */
    size_t j = 0;
    j += (size_t)snprintf(reqbody + j, sizeof(reqbody) - j, "{\"csr\":\"");
    for (const char *p = (const char *)csrbuf; *p != '\0' && j + 8 < sizeof(reqbody); p++) {
        if (*p == '\n')      j += (size_t)snprintf(reqbody + j, sizeof(reqbody) - j, "\\n");
        else if (*p == '"')  j += (size_t)snprintf(reqbody + j, sizeof(reqbody) - j, "\\\"");
        else                 reqbody[j++] = *p;
    }
    snprintf(reqbody + j, sizeof(reqbody) - j, "\",\"ttl\":\"8760h\"}");

    st = https_request(&u, "POST", "/ca/sign", headers, reqbody, body, sizeof(body));
    if (st != 200) {
        /* A revoked device is refused here, permanently — the CA has no un-revoke. */
        qz_log("ERR", "POST /ca/sign -> HTTP %d: %.240s", st, body);
        return -1;
    }
    static char cert[16384];
    if (json_str(body, "certificate", cert, sizeof(cert)) != 0) {
        qz_log("ERR", "no certificate in the reply: %.200s", body); return -1;
    }
    if (write_file(cert_path, cert, 0644) != 0) return -1;
    qz_log("STEP", "sign      ttl=8760h");

    /* 7. verify what we were given actually chains to the root we fetched. */
    char cn[QZ_MAX_ID] = {0};
    qz_cert_cn(cert_path, cn, sizeof(cn));
    qz_log("STEP", "verify    CN=%s", cn);
    qz_log("DONE", "artifacts in %s", out_dir);
    if (strcmp(cn, id) != 0)
        qz_log("WARN", "the issued CN '%s' differs from the id '%s'", cn, id);
    return 0;
}
