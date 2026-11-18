# BLG 337E Project 1 – Reliable Data Transfer over UDP

Sender and receiver implementing **Stop-and-Wait**, **Go-Back-N** and **Selective Repeat**
over UDP, with a self-implemented CRC-32 and an adaptive RTO (RFC 6298).
C, POSIX, standard C library only. Tested on Ubuntu 24.04 / gcc 13.

> Status: endpoints (sender, receiver, packet format, CRC-32, RTO) are done.
> The impairment emulator and the analysis scripts are developed separately.

## Build

```bash
make            # builds sender, receiver, test_crc
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
./sender   -p gbn -w 16 -k 7 -s 1024 -f input.bin -P 5000 -L sender.log
sha256sum input.bin received.bin
```

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
| `test_crc.c` | CRC and packet unit tests |
| `tests/` | relay and end-to-end test script |
