# Reliable Data Transfer over UDP

Sender and receiver implementing **Stop-and-Wait**, **Go-Back-N** and **Selective Repeat**
over UDP, with a self-implemented CRC-32 and an adaptive RTO (RFC 6298).
C, POSIX, standard C library only. Tested on Ubuntu 24.04 / gcc 13.

Three C programs (sender, impairment emulator, receiver) plus Python scripts that
run all experiments and compute every metric from the logs.

## Build

```bash
make            # builds sender, receiver, emulator, test_crc
make test       # unit tests + end-to-end tests (needs python3)
```

## Run

```
sender  ──▶  emulator  ──▶  receiver
sender  ◀──  emulator  ◀──  receiver
```
The sender only knows the emulator's port; the receiver answers to whatever
address sent it the packet, i.e. the emulator. Start the receiver first.

```bash
./receiver -p gbn -w 16 -k 7 -l 6000 -o received.bin -L receiver.log
./emulator -l 5000 -r 6000 -s experiments/scenarios/baseline.txt -L emulator.log &
./sender   -p gbn -w 16 -k 7 -s 1024 -f input.bin -P 5000 -L sender.log
kill %1                      # the emulator runs until stopped (or --idle MS)
sha256sum input.bin received.bin
```
Any emulator parameter can also be changed on the command line, e.g.
`./emulator -l 5000 -r 6000 --set data_loss=0.1 --set delay_ms=100 --seed 7`.

### Sender options
| Option | Meaning | Default |
|---|---|---|
| `-f, --file FILE` | file to send | required |
| `-P, --dest-port PORT` | emulator (or receiver) port | required |
| `-H, --dest-host IP` | destination address | 127.0.0.1 |
| `-l, --local-port PORT` | local port to bind (0 = any) | 0 |
| `-p, --proto sw\|gbn\|sr` | ARQ protocol | sw |
| `-w, --window W` | window size (sw forces 1) | 16 |
| `-k, --bits K` | sequence-number bits, numbers are mod 2^k | 7 |
| `-s, --payload BYTES` | max payload per packet (≤ 1460) | 1024 |
| `-L, --log PATH` | event log | sender.log |
| `--rto-init MS` | RTO before the first RTT sample | 1000 |
| `--rto-min MS` / `--rto-max MS` | RTO bounds | 200 / 60000 |
| `--fin-retries N` | FIN attempts before giving up | 10 |

### Receiver options
| Option | Meaning | Default |
|---|---|---|
| `-l, --port PORT` | port to listen on | required |
| `-o, --out FILE` | output file | required |
| `-p, --proto sw\|gbn\|sr` | must match the sender | sw |
| `-w, --window W` | receive window (SR only, must match sender) | 16 |
| `-k, --bits K` | must match the sender | 7 |
| `-L, --log PATH` | event log | receiver.log |
| `--linger MS` | keep answering FIN after the transfer | 2000 |
| `--idle MS` | give up after this long without packets (0 = never) | 30000 |

Exit codes: sender `0` ok, `2` FINACK never received. Receiver `0` ok, `3` idle timeout (file incomplete).

### Emulator options
| Option | Meaning | Default |
|---|---|---|
| `-l, --listen PORT` | port the sender sends to | required |
| `-r, --recv-port PORT` | receiver's port | required |
| `-H, --recv-host IP` | receiver's address | 127.0.0.1 |
| `-s, --scenario FILE` | scenario file (below) | none = baseline |
| `-S, --set KEY=VALUE` | set one parameter, overrides the file; repeatable | |
| `--seed N` | PRNG seed (same as `--set seed=N`) | 1 |
| `-L, --log PATH` | event log | emulator.log |
| `--idle MS` | exit after MS without packets | 0 = run until SIGINT/SIGTERM |

## Impairment emulator

```
sender ──▶ [listen port] ─ fwd link: queue → L/R → delay±jitter ─▶ receiver
sender ◀── [listen port] ◀─ rev link: queue → L/R → delay±jitter ─ [own port] ◀── receiver
```
Two sockets: one bound to `--listen` (faces the sender), one on a free port
(faces the receiver; the receiver replies to it, which is how the direction is
known). Every arriving datagram goes through, in this order:

1. **Scenario actions** for that specific packet (drop / delay / corrupt / dup).
2. **Loss**: `data_loss` drops DATA packets (fwd), `ack_loss` drops ACKs (rev).
   Lost packets never reach the link.
3. **Corruption**: with probability `corrupt`, one uniformly chosen bit of the
   datagram (header or payload) is flipped.
4. **Duplication**: with probability `dup`, an exact copy is queued right behind
   the original (it also uses the link).
5. **Reordering**: with probability `reorder`, the packet is held back; when the
   next packet of that direction arrives, that one is queued first and the held one
   right after it. If nothing follows within `max_hold_ms` the held packet is
   released anyway (only matters at the very end of a transfer). While a packet is
   held, the next packet is not itself held.
6. **Link**: FIFO queue of `queue_pkts` packets (including the one being sent,
   tail drop when full). Packets leave one at a time, each taking `L·8/R` s,
   L = datagram bytes (12-byte header + payload), R = `rate_bps`.
7. **Propagation**: delivered `delay_ms` + U(−`jitter_ms`, +`jitter_ms`) after its
   transmission ends. Order is kept: a packet is never delivered before the previous
   packet of its direction (except packets delayed by a scenario `delay` action,
   which are meant to arrive late).

**Reproducibility.** Every random decision is a pure function of
(seed, direction, arrival index n of the packet in that direction, kind of
decision), computed with the splitmix64 hash. The n-th packet of a direction
therefore gets the same fate for a given seed, independent of the other direction,
of which impairments are enabled and of OS scheduling.

### Scenario file
One `key value` per line, `#` starts a comment. A key without prefix sets both
directions; `fwd.` / `rev.` sets one direction (fwd = sender→receiver).

| Key | Meaning | Baseline |
|---|---|---|
| `rate_bps` | link rate, bit/s (0 = infinite) | 1000000 |
| `delay_ms` | propagation delay | 20 |
| `jitter_ms` | uniform jitter j, offset in [−j, +j] | 0 |
| `queue_pkts` | queue capacity, packets | 128 |
| `data_loss` | DATA loss probability (fwd only) | 0 |
| `ack_loss` | ACK loss probability (rev only) | 0 |
| `corrupt`, `dup`, `reorder` | probabilities, both directions | 0 |
| `max_hold_ms` | longest time a reordered packet is held | 1000 |
| `seed` | PRNG seed | 1 |

Per-packet actions: `<drop|delay|corrupt|dup> <data|ack|fin|finack> <match> [ms M]`,
where `<match>` is `seq S[-T] [occ N]` (the N-th time a packet with that number
arrives, default the first) or `index I[-J]` (the I-th packet of that type to arrive,
0-based, counting retransmissions):
```
drop data seq 12              # first transmission of DATA 12
drop data seq 12 occ 2        # ...and its first retransmission
delay ack seq 30 ms 200       # ACK 30 arrives 200 ms late
drop ack index 0-6            # the first seven ACKs
```

### Emulator log
```
time_us,event,dir,pid,type,seq,bytes,qlen,tx_start_us,tx_end_us,deliver_us,info
```
`pid` = emulator-wide packet id (a duplicate gets its own pid, `info=orig=<pid>`).
Events: `RX` (arrival, `info=idx=n`), `DROP_LOSS`, `DROP_ACKLOSS`, `DROP_ACTION`,
`DROP_QUEUE`, `DELAY_ACTION`, `CORRUPT` (`info=bit=b`), `DUP`, `HOLD`,
`RELEASE` (`info=next|timeout`), `ENQ` (queue length and the computed transmission
start/end and delivery time), `DELIVER` (`info=late_us=` scheduling error, typically
~0.15 ms). The `#` lines record every parameter of the run.

## Experiments and analysis

```bash
make
python3 experiments/run_experiments.py          # all 5 experiments (~20 min)
python3 experiments/run_experiments.py 2 --only sr   # a subset
python3 experiments/analyze.py                  # metrics, tables, plots
```
`run_experiments.py` contains the whole run plan (parameters, seeds). For each run it
writes `results/exp<N>/<run>/` with `scenario.txt`, the three logs, `stdout.txt` and
`run.json` (all parameters, the exact command lines, exit codes and both SHA-256
hashes). The test file `experiments/input.bin` is 256 KiB of pseudo-random bytes
(generated with seed 337 if missing). `analyze.py` writes `results/summary/exp<N>.csv`
(every metric of every run), `results/summary/tables.md` and `results/plots/*.png`.
Metric definitions are at the top of `analyze.py`.

## Packet format

12-byte binary header, all multi-byte fields big-endian (network order):

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `type` | 0 = DATA, 1 = ACK, 2 = FIN, 3 = FINACK |
| 1 | 1 | `flags` | reserved, always 0 |
| 2 | 2 | `len` | payload length in bytes |
| 4 | 4 | `num` | sequence number (DATA, FIN) or acknowledgment number (ACK, FINACK); only the low k bits are used |
| 8 | 4 | `crc` | CRC-32 (IEEE 802.3, reflected poly 0xEDB88320, init/final XOR 0xFFFFFFFF) over header + payload, computed with this field set to 0 |
| 12 | len | payload | file bytes (DATA only) |

A packet is discarded silently if the CRC does not match, if `len` disagrees with the
datagram size, or if `type` is unknown. CRC check value: `crc32("123456789") = 0xCBF43926`.

**Extra packet types (justification).** The transfer has to end somehow: the receiver
does not know the file size in advance. `FIN` (seq = N mod 2^k, sent after every data
packet is acknowledged) tells it the file is complete; `FINACK` confirms it. A separate
type is used instead of a normal ACK so a delayed data ACK with the same number can
never be mistaken for the end of the transfer. After FIN the receiver lingers
(`--linger`) and re-answers retransmitted FINs, in case the FINACK is lost.

## Protocol behaviour

Packets have an absolute index 0..N-1 internally; `index mod 2^k` goes on the wire.
An incoming number is mapped back by its distance from the window start,
`d = (num − base) mod 2^k`.

| | Sender | Receiver | ACK meaning |
|---|---|---|---|
| **Stop-and-Wait** | GBN machinery with W = 1 | accepts only the expected seq | seq of the packet received |
| **Go-Back-N** | one timer for the oldest unACKed packet; on timeout resend the whole window | accepts only the expected seq, otherwise discards and re-ACKs the last in-order packet | cumulative: "everything up to and including `num`" |
| **Selective Repeat** | one timer per packet, only the expired packet is resent | buffers anything in `[E, E+W)`, ACKs each packet individually, re-ACKs old duplicates in `[E−W, E)`, delivers in order | individual: "packet `num`" |

**Window limits** (sender warns but allows larger windows, for experiment 5):
Go-Back-N needs `W ≤ 2^k − 1`, Selective Repeat needs `W ≤ 2^(k−1)`.

**RTO (RFC 6298).** First sample: SRTT = R, RTTVAR = R/2. Then
RTTVAR = ¾·RTTVAR + ¼·|SRTT − R|, SRTT = ⅞·SRTT + ⅛·R, RTO = SRTT + 4·RTTVAR,
clamped to [200 ms, 60 s] (RFC says 1 s minimum; 200 ms like Linux, because the
baseline RTT is only ~50 ms). Initial RTO 1 s. On timeout RTO doubles.
* **Karn's rule:** no RTT sample from a packet that was ever retransmitted.
* **Backoff reset on progress:** when an ACK acknowledges new data the backoff is
  cleared (RTO = SRTT + 4·RTTVAR) even if Karn forbids a sample. Without this, GBN at
  10 % loss retransmits nearly every packet, never gets a valid sample, and the RTO
  climbed to 51 s in testing.
* SR: if several packet timers expire at the same moment, the RTO is doubled once,
  not once per packet.

## Log format

CSV with a header line; lines starting with `#` are run metadata (protocol, W, k,
payload, file size, number of packets, …). Timestamps are `CLOCK_MONOTONIC` in
microseconds, shared by all processes on the machine.

```
time_us,event,seq,abs,bytes,rtt_us,srtt_us,rto_us
```
`seq` = number on the wire, `abs` = absolute packet index, `bytes` = payload bytes
(add 12 for the header to get bytes on the wire). Empty column = not applicable.

| Sender event | Meaning |
|---|---|
| `SEND` / `RETX` | data packet sent for the first time / retransmitted (rto_us = current RTO) |
| `ACK` | ACK acknowledging new data (abs = newest packet it covers) |
| `ACK_DUP` | ACK that acknowledges nothing new |
| `RTT` | valid sample: rtt_us, new srtt_us and rto_us |
| `RTT_SKIP` | sample rejected by Karn's rule |
| `TIMEOUT` | timer expired for packet abs (rto_us = RTO after backoff) |
| `CORRUPT` | packet with bad CRC dropped |
| `DATA_DONE` | last data packet ACKed; `rtt_us` column = **completion time**, bytes = file size |
| `FIN` / `FIN_RETX` / `FINACK` / `FIN_GIVEUP` | connection teardown |

| Receiver event | Meaning |
|---|---|
| `RX_DATA` | new in-order packet (delivered) |
| `RX_BUF` | SR: new out-of-order packet buffered |
| `RX_DUP` | duplicate (already received), re-ACKed |
| `RX_DISCARD` | GBN/SW: out-of-order, discarded |
| `RX_IGNORE` | SR: outside both windows, dropped |
| `ACK_TX` / `FINACK_TX` | acknowledgment sent |
| `CORRUPT` | bad CRC, dropped |
| `FIN_RX` / `FIN_DUP` / `FIN_EARLY` | FIN handling |
| `IDLE_TIMEOUT` / `DONE` | end; abs = packets delivered, bytes = bytes written |

## Tests

`tests/run_tests.sh` sends a 256 KiB file through `tests/relay.py` (a small
Python relay used **only for testing** the endpoints; it is not the project's
emulator) and checks the SHA-256 of every received file:
clean network, 10 % loss, 5 % corruption / reordering / duplication / ACK loss,
everything at once, k = 3 wrap-around, and the window limits
(GBN k=3 W=7 ok / W=8 corrupt, SR k=3 W=4 ok / W=5 corrupt, SR k=4 W=8 ok / W=9 corrupt).

## Files

| File | Content |
|---|---|
| `common.h` | packet layout, constants, shared prototypes |
| `crc32.c` | table-driven CRC-32 |
| `packet.c` | pack / unpack / verify |
| `util.c` | timestamps, logging, RTO estimator |
| `sender.c`, `receiver.c` | the endpoints |
| `emulator.c` | impairment emulator |
| `test_crc.c` | CRC and packet unit tests |
| `tests/` | relay and end-to-end test script |
| `experiments/` | run plan + runner, analysis, scenario examples, test file |
| `results/` | logs, scenario files and hashes of every run; summary tables and plots |
