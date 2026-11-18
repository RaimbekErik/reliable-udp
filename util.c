/* util.c - logging, timestamps, protocol names, RTO estimator */
#include <string.h>
#include <time.h>
#include "common.h"

/* ---------------------------------------------------------------- misc */
int proto_parse(const char *s)
{
    if (!strcmp(s, "sw"))  return PROTO_SW;
    if (!strcmp(s, "gbn")) return PROTO_GBN;
    if (!strcmp(s, "sr"))  return PROTO_SR;
    return -1;
}

const char *proto_name(int p)
{
    return p == PROTO_SW ? "sw" : p == PROTO_GBN ? "gbn" : "sr";
}

/* ---------------------------------------------------------------- log */
uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

FILE *log_open(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) {
        setvbuf(f, NULL, _IOFBF, 1 << 16);
        fprintf(f, "time_us,event,seq,abs,bytes,rtt_us,srtt_us,rto_us\n");
    }
    return f;
}

static void col(FILE *f, long v)
{
    if (v >= 0) fprintf(f, ",%ld", v);
    else        fputc(',', f);
}

void log_ev(FILE *f, const char *ev, long seq, long abs, long bytes,
            long rtt_us, long srtt_us, long rto_us)
{
    if (!f) return;
    fprintf(f, "%llu,%s", (unsigned long long)now_us(), ev);
    col(f, seq); col(f, abs); col(f, bytes);
    col(f, rtt_us); col(f, srtt_us); col(f, rto_us);
    fputc('\n', f);
}

/* ------------------------------------------------- RTO, RFC 6298 */
/* alpha = 1/8, beta = 1/4, K = 4. Clock granularity G is ~1 us, ignored. */
static uint64_t clamp(const rto_est *r, double v)
{
    if (v < (double)r->min_us) return r->min_us;
    if (v > (double)r->max_us) return r->max_us;
    return (uint64_t)v;
}

void rto_init(rto_est *r, uint64_t init_us, uint64_t min_us, uint64_t max_us)
{
    r->have_sample = 0;
    r->srtt_us = r->rttvar_us = 0;
    r->min_us = min_us;
    r->max_us = max_us;
    r->rto_us = init_us;
}

void rto_sample(rto_est *r, uint64_t rtt_us)
{
    double R = (double)rtt_us;
    if (!r->have_sample) {                       /* RFC 6298 (2.2) */
        r->srtt_us   = R;
        r->rttvar_us = R / 2;
        r->have_sample = 1;
    } else {                                     /* RFC 6298 (2.3) */
        double err = r->srtt_us - R;
        if (err < 0) err = -err;
        r->rttvar_us = 0.75 * r->rttvar_us + 0.25 * err;   /* uses OLD srtt */
        r->srtt_us   = 0.875 * r->srtt_us + 0.125 * R;
    }
    r->rto_us = clamp(r, r->srtt_us + 4 * r->rttvar_us);
}

void rto_backoff(rto_est *r)                     /* RFC 6298 (5.5) */
{
    r->rto_us = clamp(r, 2.0 * (double)r->rto_us);
}

/* Called when an ACK acknowledges NEW data. Undo the exponential backoff and
 * go back to SRTT + 4*RTTVAR (same as Linux TCP clearing icsk_backoff).
 * Without this, Go-Back-N under loss retransmits almost every packet, Karn's
 * rule then rejects every RTT sample and the backed-off RTO grows forever. */
void rto_progress(rto_est *r)
{
    if (r->have_sample)
        r->rto_us = clamp(r, r->srtt_us + 4 * r->rttvar_us);
}
