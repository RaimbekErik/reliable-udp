# Theoretical derivations (Experiments 1, 2, 3, 5)

These are the formulas implemented in `experiments/analyze.py` (functions
`theory_ideal`, `theory_loss`, `seq_limit`). The measured values they are
compared with are in `results/summary/tables.md` and `results/plots/`.

## 0. Notation and baseline numbers

| Symbol | Meaning | Baseline value |
|---|---|---|
| F | file size | 262 144 B (256 KiB) |
| P | maximum payload | 1024 B |
| H | header | 12 B |
| N | number of data packets = ⌈F/P⌉ | 256 |
| R | link rate per direction | 10⁶ bit/s |
| D | one-way propagation delay | 20 ms |
| T_f | transmission time of a data packet = (P+H)·8/R | 8 288 bits / 10⁶ = **8.288 ms** |
| T_a | transmission time of an ACK = H·8/R | 96 bits / 10⁶ = **0.096 ms** |
| RTT₀ | round-trip time of one packet with empty queues = T_f + D + T_a + D | **48.384 ms** |
| M | size of the sequence space = 2^k | 128 |

RTT₀ is measured from the moment the sender hands a data packet to the
emulator until its ACK is back: the packet is transmitted (T_f), propagates (D),
the ACK is transmitted (T_a) and propagates (D). Processing times of the
programs are ignored (they are ~0.1–0.3 ms per packet in the measurements).

The best goodput any protocol can reach is the link rate times the fraction of
each packet that is payload: R·P/(P+H) = 10⁶·1024/1036 = **988.4 kbit/s**.

## 1. Experiment 1 – ideal network

**Stop-and-Wait.** Exactly one packet is outstanding. Packet i+1 is sent the
moment the ACK of packet i arrives, so every packet occupies one RTT₀:

  T_SW = N · RTT₀ = 256 · 48.384 ms = **12.386 s**

  goodput = 8F / T = 2 097 152 bit / 12.386 s = **169.3 kbit/s**

  U_SW = (time the link sends new data) / T = N·T_f / (N·RTT₀) = T_f / RTT₀ = 8.288/48.384 = **0.171**

**Sliding window (GBN and SR are identical without loss).** The sender may have
W unacknowledged packets. Two regimes:

*Window-limited* (W·T_f < RTT₀): the sender emits W packets back to back
(W·T_f), then must wait for the first ACK, which comes RTT₀ after the first
packet was sent. From then on every returning ACK releases one packet, so the
pattern repeats every RTT₀: packet i is sent at ⌊i/W⌋·RTT₀ + (i mod W)·T_f. The
last packet (i = N−1) is ACKed RTT₀ after it is sent:

  T = ⌊(N−1)/W⌋·RTT₀ + ((N−1) mod W)·T_f + RTT₀,  U ≈ W·T_f/RTT₀

*Link-limited* (W·T_f ≥ RTT₀): the first ACK returns before the window is
used up, so the link never idles; packets leave back to back:

  T = (N−1)·T_f + RTT₀,  U = N·T_f / T

The border is the bandwidth-delay product measured in packets:

  W* = RTT₀ / T_f = 48.384 / 8.288 = **5.84**  →  every W ≥ 6 saturates the link.

| W | regime | T theory | goodput theory | U theory |
|---|---|---|---|---|
| 1 | window | 12.386 s | 169.3 kbit/s | 0.171 |
| 2 | window | 127·48.384 + 8.288 + 48.384 = 6.201 s | 338.2 kbit/s | 0.342 |
| 4 | window | 63·48.384 + 3·8.288 + 48.384 = 3.121 s | 671.8 kbit/s | 0.680 |
| 8, 16, 32 | link | 255·8.288 + 48.384 = 2.162 s | 970.1 kbit/s | 0.981 |

U stays slightly below 1 for large W only because of the final RTT₀ in which
the link is idle while the last ACK returns: N·T_f/((N−1)·T_f + RTT₀) = 0.981.
Increasing W beyond W* cannot help: the link, not the protocol, is the bottleneck.

## 2. Experiment 2 – data packet loss (probability p, ACKs not lost)

Every transmission of a data packet is lost independently with probability p.
The number of transmissions a packet needs is geometric: P(k failures) =
(1−p)·p^k, mean 1/(1−p).

**RTO.** At the baseline, RTT samples are ≈ 48 ms and almost constant, so
RTTVAR → 0 and SRTT + 4·RTTVAR ≈ 50 ms, below the 200 ms minimum: the RTO in
force is the minimum, RTO ≈ 200 ms (for windowed protocols, queueing makes
RTT samples larger, so the analysis uses the RTO measured at timeouts). After a
timeout the RTO doubles (RFC 6298 §5.5) and is reset when new data is ACKed.

**Stop-and-Wait.** A packet that is lost k times waits RTO, 2·RTO, …,
2^(k−1)·RTO before the copy that gets through, which then takes RTT₀:

  E[time per packet] = RTT₀ + RTO·E[2^k − 1] = RTT₀ + RTO·( (1−p)/(1−2p) − 1 ) = RTT₀ + RTO · p/(1−2p)

using E[2^k] = Σ(1−p)(2p)^k = (1−p)/(1−2p) (valid for p < ½).

  T_SW = N · (RTT₀ + RTO·p/(1−2p))
  p = 5 %:  256 · (48.384 + 200·0.0556) = **15.23 s**;  p = 10 %: 256 · (48.384 + 25) = **18.79 s**

Retransmission overhead: each transmission is a retransmission exactly when the
previous one was lost, so the fraction of retransmitted bytes is **p**.

**Selective Repeat.** Only lost packets are resent, so the overhead is again
**p**. The time cost comes from stalls: when the packet at the window base is
lost, the sender keeps sending the next W−1 packets (W·T_f of link time), then
the window is full and the link idles until the retransmission, sent RTO after
the original, is ACKed one RTT later. The window then jumps forward because the
other packets were already ACKed individually. Stall per loss:

  S_SR = max(0, RTO + RTT − W·T_f),  E[number of losses] = N·p/(1−p)

  T_SR = (N−1)·T_f + RTT₀ + N·p/(1−p) · S_SR

With RTO = 200 ms, RTT ≈ 48 ms, W = 16 (W·T_f = 132.6 ms): S_SR ≈ 116 ms, so
p = 10 % gives 2.16 + 28.4·0.116 ≈ 5.5 s. If W·T_f ≥ RTO + RTT the window never
blocks and SR loses only the link time of the retransmissions.

**Go-Back-N.** The receiver discards everything after a lost packet, and on a
timeout the sender resends the whole window. In terms of the stream of packets,
a loss at packet j means the transmission restarts from j one RTO after j was
sent, so every loss in the (restarted) stream shifts the rest of the transfer by
one RTO, doubled if the same packet is lost again:

  T_GBN = (N−1)·T_f + RTT₀ + N · RTO · p/(1−2p)

Overhead: each loss makes (up to) W packets be sent again. With a full window,
the expected number of transmissions per delivered packet is (Stallings)

  E_GBN = (1 − p + p·W) / (1 − p),  overhead = 1 − 1/E_GBN

  W = 16: p = 5 % → E = 1.79, overhead **0.44**;  p = 10 % → E = 2.78, overhead **0.64**

**Why the costs differ.** SW pays a full RTO of idle link per loss but resends
1 packet; SR resends 1 packet and idles only for RTO + RTT − W·T_f; GBN resends
the whole window (≈ W times the bytes of SR) *and* idles for an RTO per loss.
So in bytes GBN far exceeds SR = SW, and in time SW >> GBN > SR.

## 3. Experiment 3 – propagation delay

RTT₀(D) = T_f + T_a + 2D = 8.384 ms + 2D. Jitter is uniform in [−j, +j] with
mean 0 in each direction; it does not change the mean RTT (order preservation
adds a little on top) but adds variance, which raises RTTVAR and hence the RTO.

| D [ms] | RTT₀ [ms] | W needed = ⌈RTT₀/T_f⌉ | U_SW = T_f/RTT₀ | U_SR, W=16 | U_SR, W=64 |
|---|---|---|---|---|---|
| 20 | 48.4 | 6 | 0.171 | 0.981 (link) | 0.981 (link) |
| 50 | 108.4 | 14 | 0.076 | 0.951 (link) | 0.951 (link) |
| 100 | 208.4 | 26 | 0.040 | ≈ 16·8.288/208.4 = 0.64 (window) | 0.911 (link) |
| 200 | 408.4 | 50 | 0.020 | ≈ 16·8.288/408.4 = 0.32 (window) | 0.839 (link) |

(Exact values come from the formulas of §1; the "link" values are below 1
because of the idle final RTT₀, which grows with D.) The window needed to keep
the link busy grows linearly with the delay: W ≥ RTT₀/T_f = (T_f + T_a + 2D)·R/((P+H)·8).

**RTO tracking.** RFC 6298: SRTT ← ⅞·SRTT + ⅛·R', RTTVAR ← ¾·RTTVAR + ¼·|SRTT − R'|,
RTO = max(200 ms, SRTT + 4·RTTVAR). With uniform jitter on both directions the
RTT standard deviation is σ = √2 · j/√3; at D = 200 ms (j = 20 ms) σ ≈ 16 ms,
so RTO ≈ RTT + 4·(mean absolute deviation ≈ 13 ms) ≈ RTT + 50 ms: above every
sample, so no spurious timeouts are expected. For small D the 200 ms floor dominates.

**Go-Back-N (not run).** Without loss and with order-preserving jitter, GBN
sends exactly the same packets at the same times as SR (no packet is ever
retransmitted), so its completion time and utilization would equal SR's for the
same W. The difference would appear only on a timeout: a spurious timeout
(an RTT sample beyond the RTO) or a loss costs GBN a whole window — up to 64
packets at W = 64 — and a full RTO of idle time, which at D = 200 ms is ≥ 0.4 s.
Large bandwidth-delay products therefore make GBN increasingly fragile.

## 4. Experiment 5 – sequence-number limits

Sequence numbers are counted modulo M = 2^k. The receiver can only tell packets
apart by their number, so all packets it might receive at a given moment must
carry distinct numbers.

**Go-Back-N** (receiver window 1). Suppose the receiver expects packet E. The
sender's window base b can be anything in [E − W, E]: from b = E (all ACKs
arrived) down to b = E − W (all W packets arrived but every ACK was lost). In
the second case the sender retransmits the old packets E − W … E − 1. The
receiver accepts only the number E mod M, so none of these W old numbers may
equal it: the W + 1 numbers E − W … E must be different modulo M:

  W + 1 ≤ M  ⇔  **W ≤ 2^k − 1**

Failure case W = M: the receiver gets packets 0 … M−1, all ACKs are lost, the
sender times out and resends packet 0 with number 0, and the receiver — now
expecting packet M, whose number is also 0 — accepts the old payload as new.

**Selective Repeat** (receiver window W). The receiver accepts numbers in
[E, E + W − 1]. If all ACKs of the last window were lost, the sender can still
resend old packets from [E − W, E − 1]. The two ranges must not overlap modulo M:

  2W ≤ M  ⇔  **W ≤ 2^(k−1)**

Failure case W = M/2 + 1: the receiver gets packets 0 … W−1 and moves its window
to [W, 2W − 1], which modulo M contains number 0 (because 2W − 1 ≥ M). All ACKs
are lost, the sender resends packet 0, and the receiver buffers it as packet M.

**Impairment used.** Both proofs use the worst case "the receiver has the whole
window, the sender has none of its ACKs". The experiment produces exactly this:
the emulator drops every ACK of the first window (`drop ack index 0-(W-1)`), for
every W. Expected and measured (results/summary/tables.md, exp 5):

| protocol | k | largest W intact | first W that fails |
|---|---|---|---|
| GBN | 3 | 7 | 8 |
| GBN | 4 | 15 | 16 |
| SR | 3 | 4 | 5 |
| SR | 4 | 8 | 9 |

The run is deterministic (the drops are scripted, not random), so the failure is
reproduced on every run.
