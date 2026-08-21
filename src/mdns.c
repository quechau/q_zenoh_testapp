/** mdns.c — find boards without being told their address.
 *
 * Two mechanisms, in order:
 *
 *   1. an mDNS PTR query for `_zenoh._tcp`, which ACB-M answers;
 *   2. a TCP sweep of the local /24 on the zenoh port.
 *
 * The sweep exists because the mDNS answer alone cannot be trusted to say *where* the board
 * is. mDNS replies are multicast and other hosts on the segment re-announce records they have
 * cached, so the sender of a reply is often not the owner of the service — measured on this
 * bench, a reply carrying the board's records arrived from a different machine entirely, and
 * dialling that machine got connection refused. Rather than parse ever more of the DNS wire
 * format to work around it, the sweep asks the only question that actually matters: who is
 * listening for zenoh on this network?
 *
 * mDNS is still used, for the peer id: the reply carries `acbm-<mac>` and the sweep does not.
 *
 * Why this matters at all: bench boards take their address from DHCP, so a remembered
 * `--endpoint` goes stale the moment the lease changes, and every later command then fails in
 * a way that looks like the board being down.
 */
#include "qz.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MDNS_ADDR    "224.0.0.251"
#define MDNS_PORT    5353
#define SERVICE      "_zenoh._tcp"
#define ZENOH_PORT   7447
#define SWEEP_BATCH  64          /* sockets in flight; a /24 finishes in four rounds */

/* ---------------------------------------------------------------- mDNS ids */

static size_t dns_name(uint8_t *out, const char *name)
{
    size_t n = 0;
    const char *p = name;
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63) break;
        out[n++] = (uint8_t)len;
        memcpy(out + n, p, len);
        n += len;
        if (dot == NULL) break;
        p = dot + 1;
    }
    out[n++] = 0;
    return n;
}

/** Builds the PTR question for `_zenoh._tcp.local`. Same packet for multicast and unicast. */
static size_t ptr_query(uint8_t *q, size_t cap)
{
    if (cap < 64) return 0;
    memset(q, 0, 12);
    q[5] = 1;                                   /* qdcount = 1 */
    size_t n = 12;
    char fqdn[128];
    snprintf(fqdn, sizeof(fqdn), "%s.local", SERVICE);
    n += dns_name(q + n, fqdn);
    q[n++] = 0x00; q[n++] = 0x0C;               /* PTR */
    q[n++] = 0x00; q[n++] = 0x01;               /* IN  */
    return n;
}

/** First `acbm-…`/`acbl-…` label in a DNS payload. Deliberately a scrape and not a parser:
 *  the id appears in the PTR target, the SRV owner and the TXT `peer=` — any of them will do,
 *  and a full name-compression parser buys nothing here. */
static int scrape_id(const uint8_t *buf, size_t len, char *out, size_t outsz)
{
    static const char *prefixes[] = { "acbm-", "acbl-" };
    for (size_t k = 0; k < 2; k++) {
        size_t pl = strlen(prefixes[k]);
        for (size_t i = 0; i + pl < len; i++) {
            if (memcmp(buf + i, prefixes[k], pl) != 0) continue;
            size_t j = 0;
            while (i + j < len && j + 1 < outsz) {
                uint8_t c = buf[i + j];
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) break;
                out[j++] = (char)c;
            }
            out[j] = '\0';
            if (j > pl) return 0;
        }
    }
    return -1;
}

/** Asks ONE host, over unicast, who it is: `_zenoh._tcp.local` PTR straight to <ip>:5353.
 *
 * The multicast browse cannot say WHICH address owns a name, and on this bench it often says
 * nothing at all — a board that has been up for hours stops answering multicast entirely
 * (measured 2026-08-21: zero multicast replies while a unicast query to the same board came
 * back instantly with its A record). Asking each swept address directly fixes both problems
 * at once: the answer is by definition the owner's, and it works with N boards, where the old
 * "one name + one address, so they must be the same board" pairing gave up and printed
 * "(unknown)" for every board. */
static int peer_id_unicast(const char *ip, char *out, size_t outsz, unsigned timeout_ms)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t q[256];
    size_t n = ptr_query(q, sizeof(q));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(MDNS_PORT);
    if (n == 0 || inet_pton(AF_INET, ip, &to.sin_addr) != 1 ||
        sendto(sock, q, n, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        close(sock);
        return -1;
    }
    int rc = -1;
    uint64_t deadline = qz_now_ms() + timeout_ms;
    while (qz_now_ms() < deadline) {
        uint8_t buf[2048];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t got = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (got <= 0) break;
        char fs[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, fs, sizeof(fs));
        if (strcmp(fs, ip) != 0) continue;      /* only the owner's own answer counts */
        if (scrape_id(buf, (size_t)got, out, outsz) == 0) { rc = 0; break; }
    }
    close(sock);
    return rc;
}

/** Collects peer ids seen in mDNS replies. Addresses are deliberately NOT taken from here. */
static size_t mdns_peer_ids(char ids[][QZ_MAX_ID], size_t max_ids, unsigned seconds)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return 0;
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    unsigned char ttl = 2;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) { close(sock); return 0; }

    uint8_t q[256];
    size_t n = ptr_query(q, sizeof(q));
    if (n == 0) { close(sock); return 0; }

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_ADDR, &to.sin_addr);
    if (sendto(sock, q, n, 0, (struct sockaddr *)&to, sizeof(to)) < 0) { close(sock); return 0; }

    size_t found = 0;
    uint64_t deadline = qz_now_ms() + (uint64_t)seconds * 1000ULL;
    while (qz_now_ms() < deadline && found < max_ids) {
        uint8_t buf[2048];
        ssize_t got = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
        if (got <= 0) continue;
        char id[QZ_MAX_ID];
        if (scrape_id(buf, (size_t)got, id, sizeof(id)) != 0) continue;
        bool dup = false;
        for (size_t d = 0; d < found; d++) if (strcmp(ids[d], id) == 0) dup = true;
        if (!dup) snprintf(ids[found++], QZ_MAX_ID, "%s", id);
    }
    close(sock);
    return found;
}

/* --------------------------------------------------------------- TCP sweep */

#define QZ_MAX_IFACES 8

typedef struct {
    uint32_t net;
    uint32_t mask;
    char     name[IF_NAMESIZE];
} qz_iface_t;

/** Every up, non-loopback IPv4 interface with a /24-or-smaller netmask.
 *
 * All of them, not just the first: picking "the first interface" is the same trap that makes
 * the Control Engine derive the wrong identity when an Ethernet cable is plugged into a
 * WiFi-connected host. The board is often not on the interface that happens to sort first. */
static size_t local_subnets(qz_iface_t *out, size_t max_out)
{
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) return 0;
    size_t n = 0;
    for (struct ifaddrs *it = list; it != NULL && n < max_out; it = it->ifa_next) {
        if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET) continue;
        if ((it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_LOOPBACK) != 0) continue;
        if (it->ifa_netmask == NULL) continue;
        uint32_t a = ntohl(((struct sockaddr_in *)it->ifa_addr)->sin_addr.s_addr);
        uint32_t m = ntohl(((struct sockaddr_in *)it->ifa_netmask)->sin_addr.s_addr);
        if (m < 0xFFFFFF00u) continue;         /* only sweep /24 or smaller */
        bool dup = false;
        for (size_t k = 0; k < n; k++) if (out[k].net == (a & m) && out[k].mask == m) dup = true;
        if (dup) continue;
        out[n].net = a & m;
        out[n].mask = m;
        snprintf(out[n].name, sizeof(out[n].name), "%s", it->ifa_name);
        n++;
    }
    freeifaddrs(list);
    return n;
}

static size_t sweep(uint32_t net, uint32_t mask, char hits[][INET_ADDRSTRLEN], size_t max_hits)
{
    uint32_t count = (~mask) & 0xFFFFFFFFu;    /* hosts in the subnet, incl. net + broadcast */
    size_t found = 0;

    for (uint32_t base = 1; base < count && found < max_hits; base += SWEEP_BATCH) {
        int fds[SWEEP_BATCH];
        uint32_t addrs[SWEEP_BATCH];
        int nfds = 0;
        for (uint32_t k = 0; k < SWEEP_BATCH && base + k < count; k++) {
            uint32_t host = net | (base + k);
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            fcntl(fd, F_SETFL, O_NONBLOCK);
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(ZENOH_PORT);
            sa.sin_addr.s_addr = htonl(host);
            connect(fd, (struct sockaddr *)&sa, sizeof(sa));   /* EINPROGRESS is expected */
            fds[nfds] = fd;
            addrs[nfds] = host;
            nfds++;
        }

        /* Keep selecting until the whole window has elapsed. select() returns as soon as ANY
         * descriptor is ready, and closed ports answer with an instant RST — so a single call
         * returns in about a millisecond with all the failures ready and the one host that is
         * genuinely listening still mid-handshake. Evaluating once and closing everything is
         * how this missed the only board on the network. */
        bool done[SWEEP_BATCH];
        memset(done, 0, sizeof(done));
        uint64_t until = qz_now_ms() + 500;
        int remaining = nfds;
        while (remaining > 0 && qz_now_ms() < until) {
            fd_set wr;
            FD_ZERO(&wr);
            int maxfd = -1;
            for (int k = 0; k < nfds; k++) {
                if (done[k]) continue;
                FD_SET(fds[k], &wr);
                if (fds[k] > maxfd) maxfd = fds[k];
            }
            if (maxfd < 0) break;
            uint64_t left = until - qz_now_ms();
            struct timeval tv = { .tv_sec = (time_t)(left / 1000),
                                  .tv_usec = (suseconds_t)((left % 1000) * 1000) };
            if (select(maxfd + 1, NULL, &wr, NULL, &tv) <= 0) break;
            for (int k = 0; k < nfds; k++) {
                if (done[k] || !FD_ISSET(fds[k], &wr)) continue;
                done[k] = true;
                remaining--;
                int err = 0;
                socklen_t el = sizeof(err);
                getsockopt(fds[k], SOL_SOCKET, SO_ERROR, &err, &el);
                if (err == 0 && found < max_hits) {
                    struct in_addr ia = { .s_addr = htonl(addrs[k]) };
                    inet_ntop(AF_INET, &ia, hits[found], INET_ADDRSTRLEN);
                    found++;
                }
            }
        }
        for (int k = 0; k < nfds; k++) close(fds[k]);
    }
    return found;
}

/* ------------------------------------------------------------------- public */

int qz_mdns_scan(qz_ctx_t *ctx, unsigned seconds)
{
    char ids[QZ_MAX_BOARDS][QZ_MAX_ID];
    size_t nids = mdns_peer_ids(ids, QZ_MAX_BOARDS, seconds > 2 ? 2 : seconds);
    if (nids > 0) {
        char joined[512] = {0};
        for (size_t i = 0; i < nids; i++) {
            strncat(joined, ids[i], sizeof(joined) - strlen(joined) - 2);
            if (i + 1 < nids) strncat(joined, ", ", sizeof(joined) - strlen(joined) - 1);
        }
        qz_log("SCAN", "mDNS named: %s", joined);
    }

    qz_iface_t ifaces[QZ_MAX_IFACES];
    size_t nif = local_subnets(ifaces, QZ_MAX_IFACES);
    if (nif == 0) {
        qz_log("ERR", "no usable IPv4 interface to sweep");
        return -1;
    }

    char hits[QZ_MAX_BOARDS][INET_ADDRSTRLEN];
    size_t nhits = 0;
    for (size_t k = 0; k < nif && nhits < QZ_MAX_BOARDS; k++) {
        struct in_addr ni = { .s_addr = htonl(ifaces[k].net) };
        char nets[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ni, nets, sizeof(nets));
        qz_log("SCAN", "probing %s/%u on %s port %d", nets,
               (unsigned)__builtin_popcount(ifaces[k].mask), ifaces[k].name, ZENOH_PORT);
        nhits += sweep(ifaces[k].net, ifaces[k].mask, hits + nhits, QZ_MAX_BOARDS - nhits);
    }

    ctx->board_count = 0;
    for (size_t i = 0; i < nhits && ctx->board_count < QZ_MAX_BOARDS; i++) {
        qz_board_t *b = &ctx->boards[ctx->board_count++];
        memset(b, 0, sizeof(*b));
        /* Ask THIS address who it is (unicast mDNS). Falls back to the old pairing rule —
         * one name, one address, so they must belong together — and finally leaves the id
         * blank, which `discover` fills in from the announce once connected. */
        if (peer_id_unicast(hits[i], b->peer_id, sizeof(b->peer_id), 700) != 0 &&
            nids == 1 && nhits == 1) {
            snprintf(b->peer_id, sizeof(b->peer_id), "%s", ids[0]);
        }
        snprintf(b->addr, sizeof(b->addr), "tls/%s:%d", hits[i], ZENOH_PORT);
        b->last_seen_ms = qz_now_ms();
    }

    if (ctx->board_count == 0) {
        qz_log("SCAN", "nothing is listening on port %d in any local subnet", ZENOH_PORT);
        return 0;
    }
    printf("\n  %-28s %s\n", "PEER ID", "ADDRESS");
    for (size_t i = 0; i < ctx->board_count; i++)
        printf("  %-28s %s\n", ctx->boards[i].peer_id[0] ? ctx->boards[i].peer_id : "(unknown)",
               ctx->boards[i].addr);
    printf("\n");

    if (ctx->board_count == 1 && ctx->endpoint[0] == '\0') {
        snprintf(ctx->endpoint, sizeof(ctx->endpoint), "%s", ctx->boards[0].addr);
        if (ctx->boards[0].peer_id[0] != '\0')
            snprintf(ctx->board, sizeof(ctx->board), "%s", ctx->boards[0].peer_id);
        qz_log("SELECT", "endpoint=%s%s%s", ctx->endpoint,
               ctx->board[0] ? "  board=" : "", ctx->board[0] ? ctx->board : "");
    } else if (ctx->board_count > 1 && ctx->endpoint[0] == '\0') {
        /* Several boards is a normal bench now (two physical ACB-Ms, 2026-08-21). Auto-picking
         * one would silently address the wrong board, so say what was found and how to choose
         * — `connect` takes either form. */
        qz_log("SELECT", "%zu boards found — choose one: `connect %s` (or `connect %s`)",
               ctx->board_count, ctx->boards[0].addr,
               ctx->boards[0].peer_id[0] ? ctx->boards[0].peer_id : "<peer-id>");
    }
    return (int)ctx->board_count;
}
