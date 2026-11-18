/* receiver.c - reliable file receiver over UDP: Stop-and-Wait, Go-Back-N, Selective Repeat.
 *
 * E = absolute index of the next packet to deliver to the file (in order).
 *
 * SW / GBN (receive window 1):
 *   seq == E mod 2^k  -> write payload, E++, ACK seq
 *   anything else     -> discard, re-ACK last in-order packet (E-1)
 *                        (nothing sent if nothing has arrived yet)
 *
 * SR (receive window W, d = (seq - E) mod 2^k):
 *   d < W             -> in window: buffer (if new), ACK seq, deliver in order
 *   d >= 2^k - W      -> already delivered (old duplicate): re-ACK seq
 *   otherwise         -> ignore
 *   The two ranges only stay disjoint when W <= 2^(k-1).
 *
 * End of transfer: FIN with seq == E mod 2^k means every data packet was
 * delivered -> close file, answer FINACK, keep answering for --linger ms in
 * case the FINACK is lost and the FIN is retransmitted.
 */
#include <arpa/inet.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common.h"

static struct {
    int proto, W, k, port;
    const char *out, *log_path;
    uint64_t linger, idle;                    /* microseconds */
} cfg = { PROTO_SW, 16, 7, 0, NULL, "receiver.log", 2000000, 30000000 };

typedef struct {
    int      valid;
    uint16_t len;
    uint8_t  data[MAX_PAYLOAD];
} slot;

static int sock;
static FILE *lg, *out;
static uint32_t mask;
static struct sockaddr_in peer;            /* where ACKs go (the emulator) */
static socklen_t peer_len;
static int have_peer;

static void usage(const char *p)
{
    fprintf(stderr,
"usage: %s -l PORT -o OUTFILE [options]\n"
"  -l, --port PORT        UDP port to listen on (required)\n"
"  -o, --out FILE         where to write the received file (required)\n"
"  -p, --proto sw|gbn|sr  ARQ protocol (must match sender) (default sw)\n"
"  -w, --window W         receive window for sr (must match sender) (default 16)\n"
"  -k, --bits K           sequence number bits (must match sender) (default 7)\n"
"  -L, --log PATH         event log (default receiver.log)\n"
"      --linger MS        keep answering FIN this long after it (default 2000)\n"
"      --idle MS          give up after this long without packets, 0 = never\n"
"                         (default 30000)\n", p);
    exit(1);
}

static void parse_args(int argc, char **argv)
{
    static struct option opts[] = {
        {"port", 1, 0, 'l'}, {"out", 1, 0, 'o'}, {"proto", 1, 0, 'p'},
        {"window", 1, 0, 'w'}, {"bits", 1, 0, 'k'}, {"log", 1, 0, 'L'},
        {"linger", 1, 0, 1}, {"idle", 1, 0, 2}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "l:o:p:w:k:L:h", opts, NULL)) != -1) {
        switch (c) {
        case 'l': cfg.port = atoi(optarg); break;
        case 'o': cfg.out = optarg; break;
        case 'p': if ((cfg.proto = proto_parse(optarg)) < 0) usage(argv[0]); break;
        case 'w': cfg.W = atoi(optarg); break;
        case 'k': cfg.k = atoi(optarg); break;
        case 'L': cfg.log_path = optarg; break;
        case 1:   cfg.linger = strtoull(optarg, 0, 10) * 1000; break;
        case 2:   cfg.idle   = strtoull(optarg, 0, 10) * 1000; break;
        default:  usage(argv[0]);
        }
    }
    if (cfg.port <= 0 || !cfg.out) usage(argv[0]);
    if (cfg.k < 1 || cfg.k > 16) { fprintf(stderr, "k must be 1..16\n"); exit(1); }
    if (cfg.proto != PROTO_SR) cfg.W = 1;
    if (cfg.W < 1 || cfg.W > (1 << cfg.k)) {
        fprintf(stderr, "window must be 1..2^k\n"); exit(1);
    }
}

static void send_ctrl(uint8_t type, uint32_t num)
{
    if (!have_peer) return;
    uint8_t buf[HDR_SIZE];
    size_t n = pkt_pack(buf, type, num & mask, NULL, 0);
    sendto(sock, buf, n, 0, (struct sockaddr *)&peer, peer_len);
    log_ev(lg, type == PKT_ACK ? "ACK_TX" : "FINACK_TX", (long)(num & mask),
           -1, -1, -1, -1, -1);
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);
    mask = (uint32_t)((1u << cfg.k) - 1);
    const uint32_t M = mask + 1;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    me.sin_port = htons((uint16_t)cfg.port);
    if (bind(sock, (struct sockaddr *)&me, sizeof me) < 0) { perror("bind"); return 1; }
    int big = 1 << 20;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);

    out = fopen(cfg.out, "wb");
    if (!out) { perror(cfg.out); return 1; }
    lg = log_open(cfg.log_path);
    if (!lg) { perror(cfg.log_path); return 1; }
    fprintf(lg, "# role=receiver proto=%s W=%d k=%d port=%d out=%s\n",
            proto_name(cfg.proto), cfg.W, cfg.k, cfg.port, cfg.out);

    slot *win = calloc((size_t)cfg.W, sizeof *win);   /* SR reorder buffer */
    long E = 0;                                         /* next abs to deliver */
    long bytes_written = 0;
    int finished = 0;
    uint64_t last_rx = now_us(), linger_until = 0;
    int rc = 0;

    for (;;) {
        /* ----- how long may we block? */
        uint64_t now = now_us();
        if (finished && now >= linger_until) break;
        if (!finished && cfg.idle && now - last_rx >= cfg.idle) {
            log_ev(lg, "IDLE_TIMEOUT", -1, E, bytes_written, -1, -1, -1);
            fprintf(stderr, "receiver: idle timeout, file incomplete\n");
            rc = 3;
            break;
        }
        uint64_t deadline = finished ? linger_until
                          : cfg.idle ? last_rx + cfg.idle : now + 1000000;
        uint64_t wait = deadline > now ? deadline - now : 0;
        struct timeval tv = { (time_t)(wait / 1000000), (suseconds_t)(wait % 1000000) };
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(sock, &rf);
        if (select(sock + 1, &rf, NULL, NULL, &tv) <= 0) continue;

        /* ----- one datagram */
        uint8_t buf[HDR_SIZE + MAX_PAYLOAD + 64];
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        if (n < 0) continue;
        peer = from; peer_len = flen; have_peer = 1;

        pkt_hdr h;
        const uint8_t *pl;
        if (pkt_unpack(buf, (size_t)n, &h, &pl) < 0) {
            log_ev(lg, "CORRUPT", -1, -1, n, -1, -1, -1);
            continue;                                /* silently drop */
        }
        last_rx = now_us();
        uint32_t s = h.num & mask;

        /* ----- FIN */
        if (h.type == PKT_FIN) {
            if (s == ((uint32_t)E & mask)) {
                if (!finished) {
                    finished = 1;
                    fflush(out);
                    log_ev(lg, "FIN_RX", s, E, bytes_written, -1, -1, -1);
                } else {
                    log_ev(lg, "FIN_DUP", s, E, -1, -1, -1, -1);
                }
                linger_until = now_us() + cfg.linger;
                send_ctrl(PKT_FINACK, s);
            } else {
                log_ev(lg, "FIN_EARLY", s, -1, -1, -1, -1, -1);
            }
            continue;
        }
        if (h.type != PKT_DATA) continue;

        /* ----- DATA after the transfer is over: just re-ACK */
        if (finished) {
            log_ev(lg, "RX_DUP", s, -1, h.len, -1, -1, -1);
            send_ctrl(PKT_ACK, cfg.proto == PROTO_SR ? s : (uint32_t)(E - 1));
            continue;
        }

        if (cfg.proto != PROTO_SR) {
            /* ========== Stop-and-Wait / Go-Back-N ========== */
            if (s == ((uint32_t)E & mask)) {
                fwrite(pl, 1, h.len, out);
                bytes_written += h.len;
                log_ev(lg, "RX_DATA", s, E, h.len, -1, -1, -1);
                E++;
                send_ctrl(PKT_ACK, s);
            } else {
                int dup = E > 0 && s == ((uint32_t)(E - 1) & mask);
                log_ev(lg, dup ? "RX_DUP" : "RX_DISCARD", s, -1, h.len, -1, -1, -1);
                if (E > 0) send_ctrl(PKT_ACK, (uint32_t)(E - 1));
            }
        } else {
            /* ========== Selective Repeat ========== */
            uint32_t d = (s - (uint32_t)E) & mask;
            if (d < (uint32_t)cfg.W) {
                long a = E + (long)d;
                slot *sl = &win[a % cfg.W];
                if (sl->valid) {
                    log_ev(lg, "RX_DUP", s, a, h.len, -1, -1, -1);
                } else {
                    sl->valid = 1;
                    sl->len = h.len;
                    memcpy(sl->data, pl, h.len);
                    log_ev(lg, d == 0 ? "RX_DATA" : "RX_BUF", s, a, h.len, -1, -1, -1);
                }
                send_ctrl(PKT_ACK, s);
                while (win[E % cfg.W].valid) {           /* deliver in order */
                    slot *x = &win[E % cfg.W];
                    fwrite(x->data, 1, x->len, out);
                    bytes_written += x->len;
                    x->valid = 0;
                    E++;
                }
            } else if (d >= M - (uint32_t)cfg.W) {
                log_ev(lg, "RX_DUP", s, -1, h.len, -1, -1, -1);
                send_ctrl(PKT_ACK, s);
            } else {
                log_ev(lg, "RX_IGNORE", s, -1, h.len, -1, -1, -1);
            }
        }
    }

    log_ev(lg, "DONE", -1, E, bytes_written, -1, -1, -1);
    fclose(out);
    fclose(lg);
    printf("receiver: %s  %ld packets, %ld bytes written to %s%s\n",
           proto_name(cfg.proto), E, bytes_written, cfg.out,
           rc ? "  (INCOMPLETE)" : "");
    return rc;
}
