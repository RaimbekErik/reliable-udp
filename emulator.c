/* emulator.c - full-duplex network impairment emulator for the ARQ project.
 *
 *     sender --> [listen port] EMULATOR [own port] --> receiver
 *     sender <-- [listen port] EMULATOR [own port] <-- receiver
 *
 * Two sockets: one faces the sender (bound to --listen), one faces the
 * receiver (ephemeral port).  The receiver answers to whatever address sent
 * it the packet, so its ACKs come back to the second socket; that is how the
 * emulator tells the two directions apart.
 *
 * Direction 0 = "fwd" = sender -> receiver (DATA, FIN)
 * Direction 1 = "rev" = receiver -> sender (ACK, FINACK)
 *
 * What happens to a packet that arrives (in this order):
 *   1. per-packet scenario actions (drop / delay / corrupt / dup a given packet)
 *   2. random loss: data_loss for DATA in fwd, ack_loss for ACK in rev
 *   3. random corruption: flip one random bit anywhere in the datagram
 *   4. random duplication: a copy is queued right behind the original
 *   5. random reordering: the packet is held back and queued after the
 *      packet that follows it in the same direction
 *   6. link: FIFO queue (capacity queue_pkts, tail drop), one packet at a
 *      time, each takes L*8/R seconds (L = datagram bytes, R = rate_bps)
 *   7. propagation delay + uniform jitter in [-j, +j]; delivery order is kept
 *      by never delivering earlier than the previous packet of that direction
 *
 * Randomness: every random decision is a pure function of
 * (seed, direction, arrival index of the packet in that direction, kind of
 * decision), computed with the splitmix64 hash.  So the n-th packet of a
 * direction always gets the same fate for the same seed, regardless of what
 * happened in the other direction or how the OS scheduled the processes.
 *
 * C, POSIX, standard C library only.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common.h"

#define MAX_DGRAM    2048        /* larger than any packet we can see       */
#define MAX_ACTIONS  4096        /* per-packet actions in a scenario file   */
#define FWD 0                    /* sender -> receiver                      */
#define REV 1                    /* receiver -> sender                      */

/* ======================================================== configuration */

/* Parameters that exist separately for each direction. */
typedef struct {
    double   rate_bps;           /* link rate, bit/s (0 = infinitely fast)  */
    double   delay_ms;           /* propagation delay                       */
    double   jitter_ms;          /* uniform jitter in [-j, +j]              */
    int      queue_pkts;         /* FIFO capacity, packets (incl. in service) */
    double   corrupt;            /* probability of flipping one bit         */
    double   dup;                /* probability of duplicating              */
    double   reorder;            /* probability of holding back             */
} dir_cfg;

/* One "do something to a specific packet" line of the scenario file. */
enum { ACT_DROP, ACT_DELAY, ACT_CORRUPT, ACT_DUP };
typedef struct {
    int      act;                /* ACT_*                                   */
    int      type;               /* PKT_DATA / PKT_ACK / PKT_FIN / PKT_FINACK */
    int      by_index;           /* 1 = match arrival index, 0 = match seq  */
    long     lo, hi;             /* index range or seq range (inclusive)    */
    long     occ;                /* seq match: which occurrence (1 = first) */
    double   ms;                 /* ACT_DELAY: extra delay                  */
} action;

static struct {
    dir_cfg  d[2];
    double   data_loss;          /* fwd DATA loss probability               */
    double   ack_loss;           /* rev ACK loss probability                */
    double   max_hold_ms;        /* reorder: release a held packet after this
                                    long even if no packet follows it       */
    uint64_t seed;
    int      listen_port, recv_port;
    const char *recv_host, *log_path, *scenario;
    double   idle_ms;            /* exit after this long without packets (0 = never) */
} cfg;

static action acts[MAX_ACTIONS];
static long   act_seen[MAX_ACTIONS];   /* seq-match actions: occurrences so far */
static int    n_acts;

/* Set every parameter to the project's baseline. */
static void cfg_defaults(void)
{
    for (int d = 0; d < 2; d = d + 1) {
        cfg.d[d].rate_bps   = 1e6;     /* 1 Mbit/s per direction             */
        cfg.d[d].delay_ms   = 20;      /* 20 ms per direction                */
        cfg.d[d].jitter_ms  = 0;
        cfg.d[d].queue_pkts = 128;     /* 128 packets per direction          */
        cfg.d[d].corrupt    = 0;
        cfg.d[d].dup        = 0;
        cfg.d[d].reorder    = 0;
    }
    cfg.data_loss   = 0;
    cfg.ack_loss    = 0;
    cfg.max_hold_ms = 1000;
    cfg.seed        = 1;
    cfg.recv_host   = "127.0.0.1";
    cfg.log_path    = "emulator.log";
    cfg.idle_ms     = 0;
}

/* --------------------------------------------------- scenario parsing */

static int type_parse(const char *s)
{
    if (!strcmp(s, "data"))   return PKT_DATA;
    if (!strcmp(s, "ack"))    return PKT_ACK;
    if (!strcmp(s, "fin"))    return PKT_FIN;
    if (!strcmp(s, "finack")) return PKT_FINACK;
    return -1;
}

static const char *type_name(int t)
{
    switch (t) {
    case PKT_DATA:   return "DATA";
    case PKT_ACK:    return "ACK";
    case PKT_FIN:    return "FIN";
    case PKT_FINACK: return "FINACK";
    default:         return "?";
    }
}

/* "12" -> lo = hi = 12 ;  "0-6" -> lo = 0, hi = 6 */
static int parse_range(const char *s, long *lo, long *hi)
{
    char *end;
    *lo = strtol(s, &end, 10);
    if (end == s) return -1;
    if (*end == '-') {
        const char *t = end + 1;
        *hi = strtol(t, &end, 10);
        if (end == t) return -1;
    } else {
        *hi = *lo;
    }
    return (*end == '\0' && *hi >= *lo) ? 0 : -1;
}

/* Action line:  <drop|delay|corrupt|dup> <data|ack|fin|finack>
 *               (seq S[-T] [occ N] | index I[-J]) [ms M]
 * Examples:     drop data seq 12          first transmission of seq 12
 *               drop data seq 12 occ 2    its second transmission
 *               drop ack index 0-6        the first seven ACKs that arrive
 *               delay ack seq 30 ms 200   ACK 30 arrives 200 ms late       */
static int parse_action(char **tok, int nt, const char *where)
{
    if (n_acts >= MAX_ACTIONS) { fprintf(stderr, "%s: too many actions\n", where); return -1; }
    action a = {0};
    a.occ = 1;
    if      (!strcmp(tok[0], "drop"))    a.act = ACT_DROP;
    else if (!strcmp(tok[0], "delay"))   a.act = ACT_DELAY;
    else if (!strcmp(tok[0], "corrupt")) a.act = ACT_CORRUPT;
    else                                 a.act = ACT_DUP;
    if (nt < 4 || (a.type = type_parse(tok[1])) < 0) goto bad;

    int have_match = 0;
    for (int i = 2; i + 1 < nt; i = i + 2) {           /* keyword value pairs */
        if (!strcmp(tok[i], "seq") || !strcmp(tok[i], "index")) {
            a.by_index = tok[i][0] == 'i';
            if (parse_range(tok[i + 1], &a.lo, &a.hi) < 0) goto bad;
            have_match = 1;
        } else if (!strcmp(tok[i], "occ")) {
            a.occ = atol(tok[i + 1]);
        } else if (!strcmp(tok[i], "ms")) {
            a.ms = atof(tok[i + 1]);
        } else {
            goto bad;
        }
    }
    if (!have_match || (nt % 2) != 0) goto bad;        /* tokens must pair up */
    if (a.act == ACT_DELAY && a.ms <= 0) goto bad;
    acts[n_acts] = a;
    n_acts = n_acts + 1;
    return 0;
bad:
    fprintf(stderr, "%s: bad action line (see README)\n", where);
    return -1;
}

/* key value   where key may carry a "fwd." or "rev." prefix to set only one
 * direction (otherwise both directions get the value). */
static int set_param(const char *key, const char *val, const char *where)
{
    int lo = 0, hi = 1;                                /* directions to set */
    if (!strncmp(key, "fwd.", 4)) { lo = hi = FWD; key += 4; }
    else if (!strncmp(key, "rev.", 4)) { lo = hi = REV; key += 4; }
    double v = atof(val);

    for (int d = lo; d <= hi; d = d + 1) {
        if      (!strcmp(key, "rate_bps"))   cfg.d[d].rate_bps   = v;
        else if (!strcmp(key, "delay_ms"))   cfg.d[d].delay_ms   = v;
        else if (!strcmp(key, "jitter_ms"))  cfg.d[d].jitter_ms  = v;
        else if (!strcmp(key, "queue_pkts")) cfg.d[d].queue_pkts = (int)v;
        else if (!strcmp(key, "corrupt"))    cfg.d[d].corrupt    = v;
        else if (!strcmp(key, "dup"))        cfg.d[d].dup        = v;
        else if (!strcmp(key, "reorder"))    cfg.d[d].reorder    = v;
        else goto global;
    }
    return 0;
global:
    if (lo != 0 || hi != 1) goto bad;                  /* prefix on a global key */
    if      (!strcmp(key, "data_loss"))   cfg.data_loss   = v;
    else if (!strcmp(key, "ack_loss"))    cfg.ack_loss    = v;
    else if (!strcmp(key, "max_hold_ms")) cfg.max_hold_ms = v;
    else if (!strcmp(key, "seed"))        cfg.seed        = strtoull(val, 0, 10);
    else goto bad;
    return 0;
bad:
    fprintf(stderr, "%s: unknown parameter '%s'\n", where, key);
    return -1;
}

/* Split one line into whitespace-separated tokens (comments start at '#')
 * and apply it. */
static int apply_line(char *line, const char *where)
{
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    char *tok[16];
    int nt = 0;
    for (char *t = strtok(line, " \t\r\n="); t && nt < 16; t = strtok(NULL, " \t\r\n="))
        tok[nt++] = t;
    if (nt == 0) return 0;                             /* blank / comment */
    /* "corrupt" and "dup" are both parameters ("corrupt 0.05") and actions
     * ("corrupt data index 5"); a parameter line has exactly two tokens */
    if (nt > 2 && (!strcmp(tok[0], "drop") || !strcmp(tok[0], "delay") ||
                   !strcmp(tok[0], "corrupt") || !strcmp(tok[0], "dup")))
        return parse_action(tok, nt, where);
    if (nt != 2) { fprintf(stderr, "%s: expected 'key value'\n", where); return -1; }
    return set_param(tok[0], tok[1], where);
}

static void load_scenario(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[512], where[600];
    int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln = ln + 1;
        snprintf(where, sizeof where, "%s:%d", path, ln);
        if (apply_line(line, where) < 0) exit(1);
    }
    fclose(f);
}

/* ================================================================ PRNG */

/* splitmix64: a well-mixed 64-bit hash, used as a counter-based PRNG. */
static uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* Kinds of random decision; each gets its own independent stream. */
enum { R_LOSS = 1, R_CORRUPT, R_BIT, R_DUP, R_REORDER, R_JITTER };

/* Uniform number in [0, 1) for decision `kind` of the idx-th packet in
 * direction dir. */
static double rnd(int kind, int dir, uint64_t idx)
{
    uint64_t h = splitmix64(cfg.seed ^ splitmix64(((uint64_t)kind << 8 | (uint64_t)dir)));
    h = splitmix64(h ^ idx);
    return (double)(h >> 11) * (1.0 / 9007199254740992.0);   /* 53 bits / 2^53 */
}

/* ============================================================ packets */

typedef struct pkt {
    long     pid;                /* emulator-wide id, used in the log       */
    int      dir;
    int      type;               /* type as it arrived (before corruption)  */
    long     num;                /* seq / ack number as it arrived          */
    size_t   len;                /* datagram bytes                          */
    double   extra_ms;           /* delay added by a scenario action        */
    uint8_t  buf[MAX_DGRAM];
} pkt;

/* -------- event heap: packets waiting for their delivery time */
typedef struct { uint64_t t; long order; pkt *p; } ev;
static ev  *heap;
static long heap_n, heap_cap, heap_order;

static int ev_less(const ev *a, const ev *b)
{
    return a->t < b->t || (a->t == b->t && a->order < b->order);   /* FIFO on ties */
}

static void heap_push(uint64_t t, pkt *p)
{
    if (heap_n == heap_cap) {
        heap_cap = heap_cap ? heap_cap * 2 : 256;
        heap = realloc(heap, (size_t)heap_cap * sizeof *heap);
    }
    long i = heap_n;
    heap_n = heap_n + 1;
    heap[i] = (ev){ t, heap_order, p };
    heap_order = heap_order + 1;
    while (i > 0) {                                    /* sift up */
        long parent = (i - 1) / 2;
        if (!ev_less(&heap[i], &heap[parent])) break;
        ev tmp = heap[i]; heap[i] = heap[parent]; heap[parent] = tmp;
        i = parent;
    }
}

static ev heap_pop(void)
{
    ev top = heap[0];
    heap_n = heap_n - 1;
    heap[0] = heap[heap_n];
    long i = 0;
    for (;;) {                                         /* sift down */
        long l = 2 * i + 1, r = l + 1, m = i;
        if (l < heap_n && ev_less(&heap[l], &heap[m])) m = l;
        if (r < heap_n && ev_less(&heap[r], &heap[m])) m = r;
        if (m == i) break;
        ev tmp = heap[i]; heap[i] = heap[m]; heap[m] = tmp;
        i = m;
    }
    return top;
}

/* ============================================================== state */

typedef struct {
    uint64_t  last_tx_end;       /* when the link finishes its current backlog */
    uint64_t  last_deliver;      /* delivery time of the previous packet (order) */
    uint64_t *q_end;             /* ring of tx-end times = packets in the queue */
    int       q_head, q_n;
    uint64_t  arrivals;          /* arrival index counter (drives the PRNG) */
    long      type_idx[4];       /* arrival index per packet type (actions) */
    pkt      *held[2];           /* reorder: held packet (+ its duplicate) */
    int       n_held;
    uint64_t  hold_until;
} dir_state;

static dir_state ds[2];
static int  s_send, s_recv;              /* sender-facing / receiver-facing sockets */
static struct sockaddr_in sender_addr, recv_addr;
static int  have_sender;
static FILE *lg;
static long next_pid;
static volatile sig_atomic_t stop;

static void on_signal(int sig) { (void)sig; stop = 1; }

/* Log line. Columns:
 * time_us,event,dir,pid,type,seq,bytes,qlen,tx_start_us,tx_end_us,deliver_us,info */
static void elog(const char *evname, const pkt *p, long qlen, uint64_t txs,
                 uint64_t txe, uint64_t dlv, const char *info)
{
    fprintf(lg, "%llu,%s,%s,%ld,%s,%ld,%zu,", (unsigned long long)now_us(), evname,
            p->dir == FWD ? "fwd" : "rev", p->pid, type_name(p->type), p->num, p->len);
    if (qlen >= 0) fprintf(lg, "%ld", qlen);
    fputc(',', lg);
    if (txs) fprintf(lg, "%llu", (unsigned long long)txs);
    fputc(',', lg);
    if (txe) fprintf(lg, "%llu", (unsigned long long)txe);
    fputc(',', lg);
    if (dlv) fprintf(lg, "%llu", (unsigned long long)dlv);
    fprintf(lg, ",%s\n", info ? info : "");
}

/* ==================================================== the link model */

/* Put one packet on the link of its direction: FIFO queue -> transmission
 * (L/R) -> propagation delay + jitter -> delivery event. */
static void link_enqueue(pkt *p, uint64_t jitter_idx)
{
    dir_state *s = &ds[p->dir];
    const dir_cfg *c = &cfg.d[p->dir];
    uint64_t now = now_us();

    /* forget packets whose transmission has finished: they left the queue */
    while (s->q_n > 0 && s->q_end[s->q_head] <= now) {
        s->q_head = (s->q_head + 1) % c->queue_pkts;
        s->q_n = s->q_n - 1;
    }
    if (s->q_n >= c->queue_pkts) {                     /* tail drop */
        elog("DROP_QUEUE", p, s->q_n, 0, 0, 0, NULL);
        free(p);
        return;
    }

    /* transmission: starts when the link is free, lasts L*8/R seconds */
    uint64_t tx_start = s->last_tx_end > now ? s->last_tx_end : now;
    uint64_t tx_time  = c->rate_bps > 0
                      ? (uint64_t)((double)p->len * 8.0 / c->rate_bps * 1e6 + 0.5) : 0;
    uint64_t tx_end   = tx_start + tx_time;
    s->last_tx_end = tx_end;
    s->q_end[(s->q_head + s->q_n) % c->queue_pkts] = tx_end;
    s->q_n = s->q_n + 1;

    /* propagation: delay + uniform jitter in [-j, +j] */
    double j = c->jitter_ms > 0 ? (2.0 * rnd(R_JITTER, p->dir, jitter_idx) - 1.0) * c->jitter_ms : 0;
    double prop_ms = c->delay_ms + j;
    if (prop_ms < 0) prop_ms = 0;
    uint64_t deliver = tx_end + (uint64_t)(prop_ms * 1000.0 + 0.5);

    if (p->extra_ms > 0) {
        /* delayed by a scenario action: may be overtaken by later packets
         * (that is the point - it arrives as an old, delayed packet) */
        deliver += (uint64_t)(p->extra_ms * 1000.0 + 0.5);
    } else {
        /* jitter must not reorder: never deliver before the previous packet */
        if (deliver < s->last_deliver) deliver = s->last_deliver;
        s->last_deliver = deliver;
    }
    elog("ENQ", p, s->q_n, tx_start, tx_end, deliver, NULL);
    heap_push(deliver, p);
}

/* Hand a held (reordered) group of packets to the link. */
static void release_held(int dir, const char *why, uint64_t jitter_idx)
{
    dir_state *s = &ds[dir];
    for (int i = 0; i < s->n_held; i = i + 1) {
        elog("RELEASE", s->held[i], -1, 0, 0, 0, why);
        link_enqueue(s->held[i], jitter_idx);
    }
    s->n_held = 0;
}

/* ============================================== impairment pipeline */

/* Find the first scenario action that applies to packet p.
 * Seq-match actions count occurrences of their seq; the action fires on the
 * occ-th occurrence. Index-match actions use the per-type arrival index. */
static int match_action(const pkt *p, long type_index)
{
    int found = -1;
    for (int i = 0; i < n_acts; i = i + 1) {
        const action *a = &acts[i];
        if (a->type != p->type) continue;
        if (a->by_index) {
            if (found < 0 && type_index >= a->lo && type_index <= a->hi) found = i;
        } else if (p->num >= a->lo && p->num <= a->hi) {
            act_seen[i] = act_seen[i] + 1;             /* one more occurrence */
            if (found < 0 && act_seen[i] == a->occ) found = i;
        }
    }
    return found;
}

static void flip_bit(pkt *p, uint64_t idx, char *info, size_t infosz)
{
    size_t bit = (size_t)(rnd(R_BIT, p->dir, idx) * (double)(p->len * 8));
    p->buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    snprintf(info, infosz, "bit=%zu", bit);
}

/* A datagram arrived in direction dir. */
static void on_arrival(int dir, const uint8_t *buf, size_t len)
{
    dir_state *s = &ds[dir];
    const dir_cfg *c = &cfg.d[dir];
    char info[64];

    pkt *p = malloc(sizeof *p);
    p->pid = next_pid;
    next_pid = next_pid + 1;
    p->dir = dir;
    p->len = len;
    p->extra_ms = 0;
    memcpy(p->buf, buf, len);
    /* read type and number straight from the header (only used for logging,
     * matching actions and deciding loss; the emulator does not check CRC) */
    p->type = len >= HDR_SIZE && buf[0] <= PKT_FINACK ? buf[0] : -1;
    p->num  = len >= HDR_SIZE ? (long)((uint32_t)buf[4] << 24 | (uint32_t)buf[5] << 16 |
                                       (uint32_t)buf[6] << 8 | buf[7]) : -1;

    uint64_t idx = s->arrivals;                        /* PRNG index */
    s->arrivals = s->arrivals + 1;
    long tidx = -1;
    if (p->type >= 0) {
        tidx = s->type_idx[p->type];
        s->type_idx[p->type] = s->type_idx[p->type] + 1;
    }
    snprintf(info, sizeof info, "idx=%llu", (unsigned long long)idx);
    elog("RX", p, -1, 0, 0, 0, info);

    /* every random draw is made for every packet, so a decision for packet n
     * never depends on which impairments are switched on */
    double u_loss    = rnd(R_LOSS, dir, idx);
    double u_corrupt = rnd(R_CORRUPT, dir, idx);
    double u_dup     = rnd(R_DUP, dir, idx);
    double u_reorder = rnd(R_REORDER, dir, idx);

    /* 1. scenario actions on this specific packet */
    int force_corrupt = 0, force_dup = 0;
    int a = p->type >= 0 ? match_action(p, tidx) : -1;
    if (a >= 0) {
        switch (acts[a].act) {
        case ACT_DROP:
            elog("DROP_ACTION", p, -1, 0, 0, 0, NULL);
            free(p);
            return;
        case ACT_DELAY:
            p->extra_ms = acts[a].ms;
            snprintf(info, sizeof info, "ms=%g", acts[a].ms);
            elog("DELAY_ACTION", p, -1, 0, 0, 0, info);
            break;
        case ACT_CORRUPT: force_corrupt = 1; break;
        case ACT_DUP:     force_dup = 1; break;
        }
    }

    /* 2. random loss */
    if (dir == FWD && p->type == PKT_DATA && u_loss < cfg.data_loss) {
        elog("DROP_LOSS", p, -1, 0, 0, 0, NULL);
        free(p);
        return;
    }
    if (dir == REV && p->type == PKT_ACK && u_loss < cfg.ack_loss) {
        elog("DROP_ACKLOSS", p, -1, 0, 0, 0, NULL);
        free(p);
        return;
    }

    /* 3. corruption: one random bit of header or payload */
    if (force_corrupt || u_corrupt < c->corrupt) {
        flip_bit(p, idx, info, sizeof info);
        elog("CORRUPT", p, -1, 0, 0, 0, info);
    }

    /* 4. duplication: an exact copy right behind the original */
    pkt *group[2] = { p, NULL };
    int ng = 1;
    if (force_dup || u_dup < c->dup) {
        pkt *q = malloc(sizeof *q);
        *q = *p;
        q->pid = next_pid;
        next_pid = next_pid + 1;
        snprintf(info, sizeof info, "orig=%ld", p->pid);
        elog("DUP", q, -1, 0, 0, 0, info);
        group[1] = q;
        ng = 2;
    }

    /* 5. reordering */
    if (s->n_held > 0) {
        /* a packet is being held: this one is "the packet that follows it",
         * so it goes first and the held one right after it */
        for (int i = 0; i < ng; i = i + 1) link_enqueue(group[i], idx);
        release_held(dir, "next", idx);
        return;
    }
    if (u_reorder < c->reorder) {
        for (int i = 0; i < ng; i = i + 1) {
            s->held[i] = group[i];
            elog("HOLD", group[i], -1, 0, 0, 0, NULL);
        }
        s->n_held = ng;
        s->hold_until = now_us() + (uint64_t)(cfg.max_hold_ms * 1000.0);
        return;
    }

    /* 6./7. link + propagation */
    for (int i = 0; i < ng; i = i + 1) link_enqueue(group[i], idx);
}

/* Deliver every packet whose time has come. */
static void deliver_due(void)
{
    uint64_t now = now_us();
    while (heap_n > 0 && heap[0].t <= now) {
        ev e = heap_pop();
        pkt *p = e.p;
        if (p->dir == FWD) {
            sendto(s_recv, p->buf, p->len, 0, (struct sockaddr *)&recv_addr, sizeof recv_addr);
        } else if (have_sender) {
            sendto(s_send, p->buf, p->len, 0, (struct sockaddr *)&sender_addr, sizeof sender_addr);
        }
        char info[32];
        snprintf(info, sizeof info, "late_us=%lld", (long long)(now_us() - e.t));
        elog("DELIVER", p, -1, 0, 0, e.t, info);
        free(p);
    }
}

/* ================================================================ main */

static void usage(const char *p)
{
    fprintf(stderr,
"usage: %s -l LISTEN_PORT -r RECEIVER_PORT [options]\n"
"  -l, --listen PORT      port the sender sends to (required)\n"
"  -r, --recv-port PORT   receiver's port (required)\n"
"  -H, --recv-host IP     receiver's address               (default 127.0.0.1)\n"
"  -s, --scenario FILE    scenario file (parameters and per-packet actions)\n"
"  -S, --set KEY=VALUE    set one parameter, overrides the file (repeatable)\n"
"      --seed N           PRNG seed, same as --set seed=N\n"
"  -L, --log PATH         event log                        (default emulator.log)\n"
"      --idle MS          exit after MS without packets    (default 0 = never)\n"
"Defaults are the project baseline: 1 Mbit/s, 20 ms, queue 128, no impairments.\n",
            p);
    exit(1);
}

static void open_sockets(void)
{
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int big = 1 << 21;

    s_send = socket(AF_INET, SOCK_DGRAM, 0);
    me.sin_port = htons((uint16_t)cfg.listen_port);
    if (s_send < 0 || bind(s_send, (struct sockaddr *)&me, sizeof me) < 0) {
        perror("bind listen port"); exit(1);
    }
    s_recv = socket(AF_INET, SOCK_DGRAM, 0);
    me.sin_port = 0;                                   /* any free port */
    if (s_recv < 0 || bind(s_recv, (struct sockaddr *)&me, sizeof me) < 0) {
        perror("bind receiver-side socket"); exit(1);
    }
    setsockopt(s_send, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    setsockopt(s_recv, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    setsockopt(s_send, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    setsockopt(s_recv, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);

    recv_addr.sin_family = AF_INET;
    recv_addr.sin_port = htons((uint16_t)cfg.recv_port);
    if (inet_pton(AF_INET, cfg.recv_host, &recv_addr.sin_addr) != 1) {
        fprintf(stderr, "bad receiver host %s\n", cfg.recv_host); exit(1);
    }
}

static void write_header(void)
{
    fprintf(lg, "# role=emulator scenario=%s seed=%llu data_loss=%g ack_loss=%g "
                "max_hold_ms=%g listen=%d receiver=%s:%d actions=%d\n",
            cfg.scenario ? cfg.scenario : "-", (unsigned long long)cfg.seed,
            cfg.data_loss, cfg.ack_loss, cfg.max_hold_ms, cfg.listen_port,
            cfg.recv_host, cfg.recv_port, n_acts);
    for (int d = 0; d < 2; d = d + 1) {
        const dir_cfg *c = &cfg.d[d];
        fprintf(lg, "# dir=%s rate_bps=%g delay_ms=%g jitter_ms=%g queue_pkts=%d "
                    "corrupt=%g dup=%g reorder=%g\n",
                d == FWD ? "fwd" : "rev", c->rate_bps, c->delay_ms, c->jitter_ms,
                c->queue_pkts, c->corrupt, c->dup, c->reorder);
    }
    fprintf(lg, "time_us,event,dir,pid,type,seq,bytes,qlen,tx_start_us,tx_end_us,"
                "deliver_us,info\n");
}

int main(int argc, char **argv)
{
    cfg_defaults();

    /* two passes: the scenario file first, then --set overrides on top */
    static struct option opts[] = {
        {"listen", 1, 0, 'l'}, {"recv-port", 1, 0, 'r'}, {"recv-host", 1, 0, 'H'},
        {"scenario", 1, 0, 's'}, {"set", 1, 0, 'S'}, {"seed", 1, 0, 1},
        {"log", 1, 0, 'L'}, {"idle", 1, 0, 2}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}
    };
    const char *optstr = "l:r:H:s:S:L:h";
    int c;
    while ((c = getopt_long(argc, argv, optstr, opts, NULL)) != -1) {
        switch (c) {
        case 'l': cfg.listen_port = atoi(optarg); break;
        case 'r': cfg.recv_port = atoi(optarg); break;
        case 'H': cfg.recv_host = optarg; break;
        case 's': cfg.scenario = optarg; break;
        case 'L': cfg.log_path = optarg; break;
        case 2:   cfg.idle_ms = atof(optarg); break;
        case 'S': case 1: break;                       /* second pass */
        default:  usage(argv[0]);
        }
    }
    if (cfg.listen_port <= 0 || cfg.recv_port <= 0) usage(argv[0]);
    if (cfg.scenario) load_scenario(cfg.scenario);
    optind = 1;
    while ((c = getopt_long(argc, argv, optstr, opts, NULL)) != -1) {
        char line[256];
        if (c == 'S') {
            snprintf(line, sizeof line, "%s", optarg);
            if (apply_line(line, "--set") < 0) exit(1);
        } else if (c == 1) {
            snprintf(line, sizeof line, "seed %s", optarg);
            if (apply_line(line, "--seed") < 0) exit(1);
        }
    }
    for (int d = 0; d < 2; d = d + 1) {
        if (cfg.d[d].queue_pkts < 1) { fprintf(stderr, "queue_pkts must be >= 1\n"); exit(1); }
        if (cfg.d[d].jitter_ms > cfg.d[d].delay_ms)
            fprintf(stderr, "warning: jitter > delay, negative delays are clamped to 0\n");
        ds[d].q_end = calloc((size_t)cfg.d[d].queue_pkts, sizeof(uint64_t));
    }

    open_sockets();
    lg = fopen(cfg.log_path, "w");
    if (!lg) { perror(cfg.log_path); return 1; }
    setvbuf(lg, NULL, _IOFBF, 1 << 16);
    write_header();

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;                         /* no SA_RESTART: select returns */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "emulator: listening on %d, forwarding to %s:%d\n",
            cfg.listen_port, cfg.recv_host, cfg.recv_port);
    uint64_t last_rx = now_us();

    while (!stop) {
        /* when is the next thing due? (delivery or forced release of a held packet) */
        uint64_t now = now_us(), next = now + 100000;  /* wake at least every 100 ms */
        if (heap_n > 0 && heap[0].t < next) next = heap[0].t;
        for (int d = 0; d < 2; d = d + 1)
            if (ds[d].n_held > 0 && ds[d].hold_until < next) next = ds[d].hold_until;
        uint64_t wait = next > now ? next - now : 0;
        struct timeval tv = { (time_t)(wait / 1000000), (suseconds_t)(wait % 1000000) };

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s_send, &rf);
        FD_SET(s_recv, &rf);
        int nfd = (s_send > s_recv ? s_send : s_recv) + 1;
        int r = select(nfd, &rf, NULL, NULL, &tv);
        if (r < 0 && errno != EINTR) { perror("select"); break; }

        /* read everything that is waiting, in both directions */
        uint8_t buf[MAX_DGRAM];
        if (r > 0 && FD_ISSET(s_send, &rf)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t n;
            while ((n = recvfrom(s_send, buf, sizeof buf, MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fl)) >= 0) {
                sender_addr = from;                    /* replies go back here */
                have_sender = 1;
                on_arrival(FWD, buf, (size_t)n);
                last_rx = now_us();
                fl = sizeof from;
            }
        }
        if (r > 0 && FD_ISSET(s_recv, &rf)) {
            ssize_t n;
            while ((n = recvfrom(s_recv, buf, sizeof buf, MSG_DONTWAIT, NULL, NULL)) >= 0) {
                on_arrival(REV, buf, (size_t)n);
                last_rx = now_us();
            }
        }

        /* release held packets whose maximum hold time ran out */
        now = now_us();
        for (int d = 0; d < 2; d = d + 1)
            if (ds[d].n_held > 0 && ds[d].hold_until <= now)
                release_held(d, "timeout", ds[d].arrivals);

        deliver_due();

        if (cfg.idle_ms > 0 && heap_n == 0 &&
            now_us() - last_rx > (uint64_t)(cfg.idle_ms * 1000.0))
            break;
    }
    fclose(lg);
    return 0;
}
