#!/usr/bin/env python3
"""Run the experiments of BLG 337E Project 1 end to end.

For every run this script
  1. writes the emulator scenario file (all impairment parameters + seed),
  2. starts receiver -> emulator -> sender on 127.0.0.1,
  3. waits for the transfer to finish, stops the emulator,
  4. stores the three logs, the scenario, the exact command lines and the
     SHA-256 of the original and the received file in results/<exp>/<run>/.

Nothing is computed here; experiments/analyze.py reads the logs afterwards.

Usage (from the repository root, after `make`):
    python3 experiments/run_experiments.py              # all experiments
    python3 experiments/run_experiments.py 1 3          # only exp 1 and 3
    python3 experiments/run_experiments.py 2 --only gbn # runs whose name contains "gbn"
    python3 experiments/run_experiments.py --list       # print the run plan only
"""
import argparse
import hashlib
import json
import os
import random
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "results")
INPUT = os.path.join(ROOT, "experiments", "input.bin")

# ----------------------------------------------------------- baseline
BASE = dict(proto="sw", W=16, k=7, payload=1024, fin_retries=10)       # endpoint parameters
BASE_EMU = dict(rate_bps=1_000_000, delay_ms=20, jitter_ms=0, queue_pkts=128,
                data_loss=0, ack_loss=0, corrupt=0, dup=0, reorder=0)
SEEDS = [1, 2, 3]          # repetitions for the experiments with random impairments


def run_def(exp, name, seed=1, actions=(), idle_ms=30000, **params):
    """One run = endpoint parameters + emulator parameters + actions + seed."""
    ep = dict(BASE)
    emu = dict(BASE_EMU)
    for key, val in params.items():
        if key in ep:
            ep[key] = val
        elif key in emu:
            emu[key] = val
        else:
            raise KeyError(key)
    return dict(exp=exp, name=name, seed=seed, endpoint=ep, emulator=emu,
                actions=list(actions), idle_ms=idle_ms)


def plan():
    """The full list of runs, experiment by experiment."""
    runs = []

    # Experiment 1: ideal network, effect of pipelining and window size
    runs.append(run_def(1, "sw", proto="sw", W=1))
    for proto in ("gbn", "sr"):
        for W in (1, 2, 4, 8, 16, 32):
            runs.append(run_def(1, f"{proto}_W{W}", proto=proto, W=W))

    # Experiment 2: data packet loss 0 / 5 / 10 %, three seeds each
    for proto in ("sw", "gbn", "sr"):
        for loss in (0, 5, 10):
            seeds = SEEDS if loss else [1]           # loss 0 is deterministic
            for s in seeds:
                runs.append(run_def(2, f"{proto}_loss{loss}_s{s}", seed=s,
                                    proto=proto, data_loss=loss / 100))

    # Experiment 3: propagation delay with jitter = 10 % of the delay
    for delay in (20, 50, 100, 200):
        j = delay / 10
        runs.append(run_def(3, f"sw_d{delay}", proto="sw", delay_ms=delay, jitter_ms=j))
        for W in (16, 64):
            runs.append(run_def(3, f"sr_W{W}_d{delay}", proto="sr", W=W,
                                delay_ms=delay, jitter_ms=j))

    # Experiment 4: corruption, reordering, duplication, ACK loss at 5 %
    for imp in ("corrupt", "reorder", "dup", "ack_loss"):
        for proto in ("sw", "gbn", "sr"):
            for s in SEEDS:
                runs.append(run_def(4, f"{imp}_{proto}_s{s}", seed=s, proto=proto,
                                    **{imp: 0.05}))

    # Experiment 5: sequence-number limits.
    # Impairment rule (the same for every W): every ACK of the first window
    # is lost ("drop ack index 0-(W-1)"). The receiver has then accepted the
    # whole first window, but the sender times out and retransmits all of it.
    # If W exceeds the limit, the receiver takes these old packets for new
    # ones -> the received file differs from the original (SHA-256).
    limits = {"gbn": lambda k: 2 ** k - 1, "sr": lambda k: 2 ** (k - 1)}
    for proto in ("gbn", "sr"):
        for k in (3, 4):
            for W in range(1, limits[proto](k) + 2):   # up to one past the limit
                runs.append(run_def(5, f"{proto}_k{k}_W{W}", proto=proto, k=k, W=W,
                                    actions=[f"drop ack index 0-{W - 1}"],
                                    idle_ms=5000, fin_retries=4))
    return runs


# ------------------------------------------------------------ helpers
def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def make_input():
    """256 KiB of reproducible pseudo-random bytes (same file for every run)."""
    if not os.path.exists(INPUT):
        rng = random.Random(337)
        with open(INPUT, "wb") as f:
            f.write(bytes(rng.getrandbits(8) for _ in range(256 * 1024)))


def write_scenario(path, r):
    with open(path, "w") as f:
        f.write(f"# experiment {r['exp']}, run {r['name']}\n")
        f.write(f"seed {r['seed']}\n")
        for key, val in r["emulator"].items():
            f.write(f"{key} {val}\n")
        for a in r["actions"]:
            f.write(a + "\n")


PORT = [40000]             # next free port pair (incremented per run)


def execute(r):
    """Run one transfer and return a summary dict (also saved as run.json)."""
    d = os.path.join(RESULTS, f"exp{r['exp']}", r["name"])
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    scen = os.path.join(d, "scenario.txt")
    write_scenario(scen, r)

    PORT[0] = PORT[0] + 2
    rport, eport = PORT[0], PORT[0] + 1
    ep = r["endpoint"]
    recv_out = os.path.join(d, "received.bin")
    common = ["-p", ep["proto"], "-w", str(ep["W"]), "-k", str(ep["k"])]
    cmd_recv = ["./receiver", *common, "-l", str(rport), "-o", recv_out,
                "-L", os.path.join(d, "receiver.log"), "--linger", "500",
                "--idle", str(r["idle_ms"])]
    cmd_emu = ["./emulator", "-l", str(eport), "-r", str(rport), "-s", scen,
               "-L", os.path.join(d, "emulator.log")]
    cmd_send = ["./sender", *common, "-s", str(ep["payload"]), "-f", INPUT,
                "--fin-retries", str(ep["fin_retries"]),
                "-P", str(eport), "-L", os.path.join(d, "sender.log")]

    out = open(os.path.join(d, "stdout.txt"), "w")
    p_recv = subprocess.Popen(cmd_recv, cwd=ROOT, stdout=out, stderr=out)
    p_emu = subprocess.Popen(cmd_emu, cwd=ROOT, stdout=out, stderr=out)
    time.sleep(0.2)                                   # let both bind their ports
    t0 = time.time()
    try:
        rc_send = subprocess.run(cmd_send, cwd=ROOT, stdout=out, stderr=out,
                                 timeout=900).returncode
    except subprocess.TimeoutExpired:
        rc_send = "timeout"
    try:
        rc_recv = p_recv.wait(timeout=r["idle_ms"] / 1000 + 10)
    except subprocess.TimeoutExpired:
        p_recv.kill()
        rc_recv = "killed"
    p_emu.terminate()
    p_emu.wait()
    out.close()

    h_orig = sha256(INPUT)
    h_recv = sha256(recv_out) if os.path.exists(recv_out) else None
    if os.path.exists(recv_out):
        os.remove(recv_out)                           # only its hash is kept
    summary = dict(r, rc_sender=rc_send, rc_receiver=rc_recv,
                   sha256_original=h_orig, sha256_received=h_recv,
                   intact=h_orig == h_recv, wall_s=round(time.time() - t0, 2),
                   commands=dict(receiver=" ".join(cmd_recv), emulator=" ".join(cmd_emu),
                                 sender=" ".join(cmd_send)))
    with open(os.path.join(d, "run.json"), "w") as f:
        json.dump(summary, f, indent=1)
    return summary


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exps", nargs="*", type=int, help="experiments to run (default all)")
    ap.add_argument("--only", default="", help="run only names containing this text")
    ap.add_argument("--list", action="store_true", help="print the plan and exit")
    a = ap.parse_args()

    runs = [r for r in plan()
            if (not a.exps or r["exp"] in a.exps) and a.only in r["name"]]
    if a.list:
        for r in runs:
            print(f"exp{r['exp']}  {r['name']}")
        print(f"{len(runs)} runs")
        return

    subprocess.run(["make", "-s", "sender", "receiver", "emulator"], cwd=ROOT, check=True)
    make_input()
    for i, r in enumerate(runs):
        s = execute(r)
        print(f"[{i + 1:3}/{len(runs)}] exp{r['exp']} {r['name']:22} "
              f"{'intact' if s['intact'] else 'DIFFERENT'}  sender rc={s['rc_sender']} "
              f"wall={s['wall_s']}s", flush=True)


if __name__ == "__main__":
    sys.exit(main())
