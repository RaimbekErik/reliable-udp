#ifndef COMMON_H
#define COMMON_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* ---------------------------------------------------------------- packet */
#define HDR_SIZE     12
#define MAX_PAYLOAD  1460          /* 1500 MTU - 20 IP - 8 UDP - 12 our header */

enum {
    PKT_DATA   = 0,   /* sender -> receiver, carries file bytes            */
    PKT_ACK    = 1,   /* receiver -> sender                                 */
    PKT_FIN    = 2,   /* sender -> receiver, "no more data" (len = 0)       */
    PKT_FINACK = 3    /* receiver -> sender, confirms FIN                   */
};

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t len;     /* payload length */
    uint32_t num;     /* seq (DATA/FIN) or ack number (ACK/FINACK), low k bits used */
    uint32_t crc;
} pkt_hdr;            /* in-memory only, NEVER sent directly */

/* crc32.c */
uint32_t crc32(const uint8_t *data, size_t n);

/* packet.c */
size_t pkt_pack(uint8_t *buf, uint8_t type, uint32_t num,
                const uint8_t *payload, uint16_t len);   /* returns total bytes */
int    pkt_unpack(const uint8_t *buf, size_t n, pkt_hdr *h,
                  const uint8_t **payload);              /* 0 = ok, -1 = bad */

/* ------------------------------------------------------------- protocols */
enum { PROTO_SW = 0, PROTO_GBN = 1, PROTO_SR = 2 };
int         proto_parse(const char *s);    /* "sw"/"gbn"/"sr" -> enum, -1 if bad */
const char *proto_name(int p);

/* ---------------------------------------------------------------- logging */
/* CSV, one line per event. Columns (unused ones left empty):
 *   time_us,event,seq,abs,bytes,rtt_us,srtt_us,rto_us
 * Lines starting with '#' are run metadata (key=value). */
uint64_t now_us(void);                      /* CLOCK_MONOTONIC, microseconds */
FILE    *log_open(const char *path);
void     log_ev(FILE *f, const char *ev, long seq, long abs, long bytes,
                long rtt_us, long srtt_us, long rto_us);   /* -1 = empty column */

/* -------------------------------------------------------- RTO (RFC 6298) */
typedef struct {
    int      have_sample;
    double   srtt_us, rttvar_us;
    uint64_t rto_us, min_us, max_us;
} rto_est;
void rto_init(rto_est *r, uint64_t init_us, uint64_t min_us, uint64_t max_us);
void rto_sample(rto_est *r, uint64_t rtt_us);   /* new RTT measurement */
void rto_backoff(rto_est *r);                   /* on timeout: RTO *= 2 */
void rto_progress(rto_est *r);                  /* new data ACKed: clear backoff */

#endif
