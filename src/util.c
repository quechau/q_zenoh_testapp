/** util.c — hashing, encoding and small file helpers, all on Mbed TLS. */
#include "qz.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

void qz_log(const char *tag, const char *fmt, ...)
{
    /* Elapsed seconds on every line: round trips and stalls then read by eye, which is the
     * whole point of a test tool. */
    static struct timeval t0;
    if (t0.tv_sec == 0) gettimeofday(&t0, NULL);
    struct timeval now;
    gettimeofday(&now, NULL);
    double el = (now.tv_sec - t0.tv_sec) + (now.tv_usec - t0.tv_usec) / 1e6;

    printf("[%7.3fs] %-8s ", el, tag);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

void qz_hex(const uint8_t *in, size_t len, char *out)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i]     = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 0x0F];
    }
    out[2 * len] = '\0';
}

void qz_sha256(const uint8_t *in, size_t len, uint8_t out[32])
{
    mbedtls_sha256(in, len, out, 0);
}

void qz_sha256_str(const char *s, uint8_t out[32])
{
    qz_sha256((const uint8_t *)s, strlen(s), out);
}

int qz_b64(const uint8_t *in, size_t in_len, char *out, size_t out_len)
{
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, out_len, &olen, in, in_len) != 0) return -1;
    out[olen] = '\0';
    return 0;
}

int qz_hmac_sha256_b64(const char *key, const char *msg, char *out, size_t out_len)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) return -1;
    uint8_t mac[32];
    if (mbedtls_md_hmac(info, (const uint8_t *)key, strlen(key),
                        (const uint8_t *)msg, strlen(msg), mac) != 0) return -1;
    return qz_b64(mac, sizeof(mac), out, out_len);
}

uint64_t qz_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

int qz_read_file(const char *path, char *buf, size_t buf_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    size_t n = fread(buf, 1, buf_len - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

int qz_cert_cn(const char *pem_path, char *out, size_t out_len)
{
    /* Read the CN rather than trusting a flag: ADR-016 has the board compare the envelope
     * client_id against this exact string, so a mismatch is the single most common reason a
     * consumer is refused everything. */
    char pem[8192];
    int n = qz_read_file(pem_path, pem, sizeof(pem));
    if (n <= 0) return -1;

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int rc = mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, (size_t)n + 1);
    if (rc != 0) { mbedtls_x509_crt_free(&crt); return -1; }

    char subject[256];
    rc = mbedtls_x509_dn_gets(subject, sizeof(subject), &crt.subject);
    mbedtls_x509_crt_free(&crt);
    if (rc <= 0) return -1;

    const char *cn = strstr(subject, "CN=");
    if (cn == NULL) return -1;
    cn += 3;
    size_t i = 0;
    while (cn[i] != '\0' && cn[i] != ',' && i + 1 < out_len) { out[i] = cn[i]; i++; }
    out[i] = '\0';
    return 0;
}
