#!/usr/bin/env python3
"""Minimal lossy UDP relay used ONLY to test the sender/receiver.

This is NOT the project's impairment emulator (that one is written in C by
the other team member). It has no link-rate model or queue; it just applies
random/scripted loss, corruption, duplication, reordering and a fixed delay
so the ARQ code can be exercised end to end.

    sender --> [listen port] relay --> receiver
    sender <-- relay <-- receiver
"""
import argparse, heapq, random, select, socket, time

p = argparse.ArgumentParser()
p.add_argument("--listen", type=int, required=True, help="port the sender sends to")
p.add_argument("--to", type=int, required=True, help="receiver port")
p.add_argument("--delay", type=float, default=2.0, help="one-way delay, ms")
p.add_argument("--loss", type=float, default=0, help="DATA loss probability")
p.add_argument("--ackloss", type=float, default=0, help="ACK loss probability")
p.add_argument("--corrupt", type=float, default=0)
p.add_argument("--dup", type=float, default=0)
p.add_argument("--reorder", type=float, default=0)
p.add_argument("--drop-acks", default="", help="comma list of ACK indices (0-based) to drop")
p.add_argument("--drop-data", default="", help="comma list of DATA indices (0-based) to drop")
p.add_argument("--seed", type=int, default=1)
a = p.parse_args()

rng = random.Random(a.seed)
drop_acks = {int(x) for x in a.drop_acks.split(",") if x}
drop_data = {int(x) for x in a.drop_data.split(",") if x}

s_in = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s_in.bind(("127.0.0.1", a.listen))
s_out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s_out.bind(("127.0.0.1", 0))
recv_addr = ("127.0.0.1", a.to)
sender_addr = None

heap, ctr = [], 0
held = {0: None, 1: None}           # reorder hold per direction
count = {0: 0, 1: 0}                # packets seen per direction
delay = a.delay / 1000.0

def schedule(t, sock, addr, data):
    global ctr
    ctr += 1
    heapq.heappush(heap, (t, ctr, sock, addr, data))

def handle(direction, data):
    """direction 0 = sender->receiver (DATA/FIN), 1 = receiver->sender (ACK)."""
    sock, addr = (s_out, recv_addr) if direction == 0 else (s_in, sender_addr)
    idx = count[direction]; count[direction] += 1
    is_data = direction == 0 and data[0] == 0
    if direction == 0 and is_data and (idx in drop_data or rng.random() < a.loss):
        return
    if direction == 1 and (idx in drop_acks or rng.random() < a.ackloss):
        return
    if rng.random() < a.corrupt:
        b = bytearray(data); bit = rng.randrange(len(b) * 8)
        b[bit // 8] ^= 1 << (bit % 8); data = bytes(b)
    copies = 2 if rng.random() < a.dup else 1
    now = time.monotonic()
    if held[direction] is not None:          # previous packet was held back
        prev = held[direction]; held[direction] = None
        for _ in range(copies):
            schedule(now + delay, sock, addr, data)
        schedule(now + delay + 1e-6, sock, addr, prev)
        return
    if rng.random() < a.reorder:
        held[direction] = data
        schedule(now + delay + 0.05, None, direction, None)   # release timer
        return
    for _ in range(copies):
        schedule(now + delay, sock, addr, data)

while True:
    timeout = max(0, heap[0][0] - time.monotonic()) if heap else 1.0
    r, _, _ = select.select([s_in, s_out], [], [], timeout)
    for s in r:
        data, src = s.recvfrom(4096)
        if s is s_in:
            sender_addr = src
            handle(0, data)
        else:
            if sender_addr: handle(1, data)
    now = time.monotonic()
    while heap and heap[0][0] <= now:
        _, _, sock, addr, data = heapq.heappop(heap)
        if sock is None:                       # release a held packet
            d = addr
            if held[d] is not None:
                pkt = held[d]; held[d] = None
                schedule(now, *( (s_out, recv_addr) if d == 0 else (s_in, sender_addr) ), pkt)
            continue
        sock.sendto(data, addr)
