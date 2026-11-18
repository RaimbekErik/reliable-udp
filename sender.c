/* sender.c - reliable file sender over UDP: Stop-and-Wait, Go-Back-N, Selective Repeat.
 *
 * Internally every packet has an ABSOLUTE index 0..N-1; only (index mod 2^k)
 * goes on the wire.  An incoming ACK number a is mapped back to an absolute
 * index by its distance from the window base:  d = (a - base) mod 2^k.
 * If d < (packets in flight) the ACK refers to packet base+d, otherwise it is
 * an old/duplicate ACK and is ignored.  This mapping is only unambiguous when
 * W <= 2^k - 1 (GBN) or W <= 2^(k-1) (SR) - experiment 5 breaks exactly this.
 *
 * Stop-and-Wait is run as the window-1 case of the Go-Back-N machinery
 * (one packet in flight, one timer, ACK = seq of the packet received).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common.h"

typedef struct {
    uint64_t sent_us;      /* time of last (re)transmission              */
    uint64_t deadline_us;  /* SR: per-packet retransmission deadline     */
    int      retx;         /* times retransmitted (Karn: no RTT if > 0)  */
    int      acked;        /* SR: individually acknowledged              */
} pstate;

static struct {
    int proto, W, k, payload, fin_retries;
    const char *file, *host, *log_path;
    int dest_port, local_port;
    uint64_t rto_init, rto_min, rto_max;     /* microseconds */
} cfg = { PROTO_SW, 16, 7, 1024, 10, NULL, "127.0.0.1", "sender.log",
          0, 0, 1000000, 200000, 60000000 };

static int sock;
static struct sockaddr_in dest;
static FILE *lg;
static uint8_t *data;          /* whole file in memory */
static size_t   fsize;
static long     N;             /* number of data packets */
static pstate  *st;
static rto_est  rto;
static uint32_t mask;          /* 2^k - 1 */

static void usage(const char *p)
{
    fprintf(stderr,
"usage: %s -f FILE -P DEST_PORT [options]\n"
"  -f, --file FILE        file to send (required)\n"
"  -P, --dest-port PORT   UDP port of the emulator (or receiver) (required)\n"
"  -H, --dest-host IP     destination address        (default 127.0.0.1)\n"
"  -l, --local-port PORT  local UDP port to bind     (default 0 = any)\n"
"  -p, --proto sw|gbn|sr  ARQ protocol               (default sw)\n"
"  -w, --window W         window size, gbn/sr        (default 16; sw forces 1)\n"
"  -k, --bits K           sequence number bits       (default 7)\n"
"  -s, --payload BYTES    max payload per packet     (default 1024, max %d)\n"
"  -L, --log PATH         event log                  (default sender.log)\n"
"      --rto-init MS      initial RTO                (default 1000)\n"
"      --rto-min MS       lower bound on RTO         (default 200)\n"
"      --rto-max MS       upper bound on RTO         (default 60000)\n"
"      --fin-retries N    FIN attempts before giving up (default 10)\n",
            p, MAX_PAYLOAD);
    exit(1);
}

static void parse_args(int argc, char **argv)
{
    static struct option opts[] = {
        {"file", 1, 0, 'f'}, {"dest-port", 1, 0, 'P'}, {"dest-host", 1, 0, 'H'},
        {"local-port", 1, 0, 'l'}, {"proto", 1, 0, 'p'}, {"window", 1, 0, 'w'},
        {"bits", 1, 0, 'k'}, {"payload", 1, 0, 's'}, {"log", 1, 0, 'L'},
        {"rto-init", 1, 0, 1}, {"rto-min", 1, 0, 2}, {"rto-max", 1, 0, 3},
        {"fin-retries", 1, 0, 4}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "f:P:H:l:p:w:k:s:L:h", opts, NULL)) != -1) {
        switch (c) {
        case 'f': cfg.file = optarg; break;
        case 'P': cfg.dest_port = atoi(optarg); break;
        case 'H': cfg.host = optarg; break;
        case 'l': cfg.local_port = atoi(optarg); break;
        case 'p': if ((cfg.proto = proto_parse(optarg)) < 0) usage(argv[0]); break;
        case 'w': cfg.W = atoi(optarg); break;
        case 'k': cfg.k = atoi(optarg); break;
        case 's': cfg.payload = atoi(optarg); break;
        case 'L': cfg.log_path = optarg; break;
        case 1:   cfg.rto_init = strtoull(optarg, 0, 10) * 1000; break;
        case 2:   cfg.rto_min  = strtoull(optarg, 0, 10) * 1000; break;
        case 3:   cfg.rto_max  = strtoull(optarg, 0, 10) * 1000; break;
        case 4:   cfg.fin_retries = atoi(optarg); break;
        default:  usage(argv[0]);
        }
    }
    if (!cfg.file || cfg.dest_port <= 0) usage(argv[0]);
    if (cfg.k < 1 || cfg.k > 16) { fprintf(stderr, "k must be 1..16\n"); exit(1); }
    if (cfg.k < 3 || cfg.k > 8)
        fprintf(stderr, "warning: assignment range for k is 3..8\n");
    if (cfg.payload < 1 || cfg.payload > MAX_PAYLOAD) {
        fprintf(stderr, "payload must be 1..%d\n", MAX_PAYLOAD); exit(1);
    }
    if (cfg.proto == PROTO_SW) cfg.W = 1;
    long M = 1L << cfg.k;
    if (cfg.W < 1 || cfg.W > M) {
        fprintf(stderr, "window must be 1..2^k (%ld)\n", M); exit(1);
    }
    /* Allowed (experiment 5 needs it) but warned: windows beyond the safe limit */
    if (cfg.proto == PROTO_GBN && cfg.W > M - 1)
        fprintf(stderr, "warning: GBN W=%d > 2^k-1=%ld, transfer may be corrupted\n",
                cfg.W, M - 1);
    if (cfg.proto == PROTO_SR && cfg.W > M / 2)
        fprintf(stderr, "warning: SR W=%d > 2^(k-1)=%ld, transfer may be corrupted\n",
                cfg.W, M / 2);
}

static void load_file(void)
{
    FILE *f = fopen(cfg.file, "rb");
    if (!f) { perror(cfg.file); exit(1); }
    fseek(f, 0, SEEK_END);
    fsize = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(fsize ? fsize : 1);
    if (fsize && fread(data, 1, fsize, f) != fsize) { perror("read"); exit(1); }
    fclose(f);
    N = (long)((fsize + cfg.payload - 1) / cfg.payload);
    st = calloc((size_t)N + 1, sizeof *st);
}

static void open_socket(void)
{
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    me.sin_port = htons((uint16_t)cfg.local_port);
    if (bind(sock, (struct sockaddr *)&me, sizeof me) < 0) { perror("bind"); exit(1); }
    int big = 1 << 20;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)cfg.dest_port);
    if (inet_pton(AF_INET, cfg.host, &dest.sin_addr) != 1) {
        fprintf(stderr, "bad host %s\n", cfg.host); exit(1);
    }
}

/* ------------------------------------------------------------ sending */
static void transmit(long i, int is_retx)
{
    uint8_t buf[HDR_SIZE + MAX_PAYLOAD];
    size_t off = (size_t)i * cfg.payload;
    uint16_t len = (uint16_t)(fsize - off < (size_t)cfg.payload ? fsize - off
                                                                 : (size_t)cfg.payload);
    size_t n = pkt_pack(buf, PKT_DATA, (uint32_t)i & mask, data + off, len);
    sendto(sock, buf, n, 0, (struct sockaddr *)&dest, sizeof dest);

    uint64_t t = now_us();
    st[i].sent_us = t;
    st[i].deadline_us = t + rto.rto_us;
    if (is_retx) st[i].retx++;
    log_ev(lg, is_retx ? "RETX" : "SEND", (long)(i & mask), i, len, -1, -1,
           (long)rto.rto_us);
}

/* Called for every ACK that acknowledges new data (packet i is the newest
 * packet it covers). */
static void take_rtt_sample(long i)
{
    if (st[i].retx) {                     /* Karn's algorithm: ambiguous, skip */
        rto_progress(&rto);               /* ...but forward progress ends backoff */
        log_ev(lg, "RTT_SKIP", (long)(i & mask), i, -1, -1, -1, (long)rto.rto_us);
        return;
    }
    uint64_t r = now_us() - st[i].sent_us;
    rto_sample(&rto, r);
    log_ev(lg, "RTT", (long)(i & mask), i, -1, (long)r, (long)rto.srtt_us,
           (long)rto.rto_us);
}

/* ------------------------------------------------------- data transfer */
static void wait_readable(uint64_t deadline)
{
    uint64_t now = now_us();
    uint64_t wait = deadline > now ? deadline - now : 0;
    struct timeval tv = { (time_t)(wait / 1000000), (suseconds_t)(wait % 1000000) };
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(sock, &rf);
    select(sock + 1, &rf, NULL, NULL, &tv);
}

static uint64_t transfer_data(void)
{
    long base = 0, next = 0;
    uint64_t timer = 0;                    /* GBN/SW single timer, 0 = off */
    uint64_t t_first = 0;

    while (base < N) {
        /* 1. fill the window with new packets */
        while (next < N && next - base < cfg.W) {
            if (!t_first) t_first = now_us();
            transmit(next, 0);
            if (base == next) timer = now_us() + rto.rto_us;
            next++;
        }

        /* 2. wait until an ACK arrives or the earliest timer expires */
        uint64_t deadline;
        if (cfg.proto == PROTO_SR) {
            deadline = UINT64_MAX;
            for (long i = base; i < next; i++)
                if (!st[i].acked && st[i].deadline_us < deadline)
                    deadline = st[i].deadline_us;
        } else {
            deadline = timer;
        }
        wait_readable(deadline);

        /* 3. process every ACK waiting in the socket */
        uint8_t buf[HDR_SIZE + MAX_PAYLOAD];
        ssize_t n;
        while ((n = recvfrom(sock, buf, sizeof buf, MSG_DONTWAIT, NULL, NULL)) >= 0) {
            pkt_hdr h;
            if (pkt_unpack(buf, (size_t)n, &h, NULL) < 0) {
                log_ev(lg, "CORRUPT", -1, -1, n, -1, -1, -1);
                continue;
            }
            if (h.type != PKT_ACK) continue;
            uint32_t a = h.num & mask;
            long d = (long)((a - (uint32_t)base) & mask);
            long inflight = next - base;

            if (cfg.proto == PROTO_SR) {
                if (d < inflight && !st[base + d].acked) {
                    long i = base + d;
                    st[i].acked = 1;
                    log_ev(lg, "ACK", a, i, -1, -1, -1, -1);
                    take_rtt_sample(i);
                    while (base < next && st[base].acked) base++;
                } else {
                    log_ev(lg, "ACK_DUP", a, -1, -1, -1, -1, -1);
                }
            } else {                       /* GBN / SW: cumulative ACK */
                if (d < inflight) {
                    long i = base + d;
                    log_ev(lg, "ACK", a, i, -1, -1, -1, -1);
                    take_rtt_sample(i);
                    base = i + 1;
                    timer = base < next ? now_us() + rto.rto_us : 0;
                } else {
                    log_ev(lg, "ACK_DUP", a, -1, -1, -1, -1, -1);
                }
            }
        }

        /* 4. handle expired timers */
        uint64_t now = now_us();
        if (cfg.proto == PROTO_SR) {
            int backed_off = 0;
            for (long i = base; i < next; i++) {
                if (st[i].acked || st[i].deadline_us > now) continue;
                if (!backed_off) { rto_backoff(&rto); backed_off = 1; }
                log_ev(lg, "TIMEOUT", (long)(i & mask), i, -1, -1, -1, (long)rto.rto_us);
                transmit(i, 1);
            }
        } else if (timer && now >= timer) {
            rto_backoff(&rto);
            log_ev(lg, "TIMEOUT", (long)(base & mask), base, -1, -1, -1, (long)rto.rto_us);
            for (long i = base; i < next; i++)     /* go back N: resend all */
                transmit(i, 1);
            timer = now_us() + rto.rto_us;
        }
    }
    uint64_t t_done = now_us();
    long ct = N ? (long)(t_done - t_first) : 0;
    log_ev(lg, "DATA_DONE", -1, N, (long)fsize, ct, -1, -1);
    return (uint64_t)ct;
}

/* FIN handshake: FIN carries seq N mod 2^k; receiver answers FINACK. */
static int close_connection(void)
{
    uint8_t buf[HDR_SIZE + MAX_PAYLOAD];
    uint32_t fseq = (uint32_t)N & mask;
    for (int attempt = 0; attempt < cfg.fin_retries; attempt++) {
        size_t n = pkt_pack(buf, PKT_FIN, fseq, NULL, 0);
        sendto(sock, buf, n, 0, (struct sockaddr *)&dest, sizeof dest);
        log_ev(lg, attempt ? "FIN_RETX" : "FIN", fseq, N, 0, -1, -1, (long)rto.rto_us);

        uint64_t deadline = now_us() + rto.rto_us;
        while (now_us() < deadline) {
            wait_readable(deadline);
            ssize_t r;
            while ((r = recvfrom(sock, buf, sizeof buf, MSG_DONTWAIT, NULL, NULL)) >= 0) {
                pkt_hdr h;
                if (pkt_unpack(buf, (size_t)r, &h, NULL) < 0) {
                    log_ev(lg, "CORRUPT", -1, -1, r, -1, -1, -1);
                    continue;
                }
                if (h.type == PKT_FINACK && (h.num & mask) == fseq) {
                    log_ev(lg, "FINACK", fseq, N, -1, -1, -1, -1);
                    return 0;
                }
            }
        }
        rto_backoff(&rto);
    }
    log_ev(lg, "FIN_GIVEUP", fseq, N, -1, -1, -1, -1);
    return -1;
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);
    mask = (uint32_t)((1u << cfg.k) - 1);
    load_file();
    open_socket();
    rto_init(&rto, cfg.rto_init, cfg.rto_min, cfg.rto_max);

    lg = log_open(cfg.log_path);
    if (!lg) { perror(cfg.log_path); return 1; }
    fprintf(lg, "# role=sender proto=%s W=%d k=%d payload=%d file=%s file_bytes=%zu "
                "packets=%ld hdr_bytes=%d dest=%s:%d rto_init_us=%llu rto_min_us=%llu "
                "rto_max_us=%llu\n",
            proto_name(cfg.proto), cfg.W, cfg.k, cfg.payload, cfg.file, fsize, N,
            HDR_SIZE, cfg.host, cfg.dest_port, (unsigned long long)cfg.rto_init,
            (unsigned long long)cfg.rto_min, (unsigned long long)cfg.rto_max);

    uint64_t ct = transfer_data();
    int rc = close_connection();
    fclose(lg);

    printf("sender: %s W=%d k=%d  %zu bytes in %ld packets, completion %.3f s, "
           "goodput %.1f kbit/s%s\n",
           proto_name(cfg.proto), cfg.W, cfg.k, fsize, N, ct / 1e6,
           ct ? fsize * 8.0 / (ct / 1e6) / 1000 : 0.0,
           rc ? "  (FINACK never received)" : "");
    return rc ? 2 : 0;
}
