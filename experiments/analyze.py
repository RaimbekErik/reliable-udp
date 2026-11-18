#!/usr/bin/env python3
"""Compute the metrics of every run from its logs, compare them with theory
and draw the plots.

Reads   results/exp<N>/<run>/{sender,receiver,emulator}.log + run.json
Writes  results/summary/exp<N>.csv      one row per run (all metrics)
        results/summary/tables.md       the tables used in the report
        results/plots/*.png             the figures used in the report

Metric definitions (exactly what the code below computes):
  completion time  t(DATA_DONE) - t(first SEND) in the sender log, i.e. from the
                   first data packet sent until the ACK of the last data packet
  goodput          file bytes * 8 / completion time                     [bit/s]
  retx overhead    retransmitted bytes / all transmitted data bytes, both on the
                   wire (payload + 12-byte header), from SEND/RETX in sender log
  link utilization time the forward link spends transmitting NEW data packets
                   (first transmissions; retransmissions and emulator duplicates
                   excluded) / completion time. Transmission intervals
                   [tx_start, tx_end] come from the emulator's ENQ events; a data
                   packet is "new" if the sender logged it as SEND. The n-th DATA
                   arrival at the emulator is the n-th SEND/RETX of the sender
                   (one process, one socket, loopback keeps the order).
  integrity        SHA-256 of original == SHA-256 of received file (run.json)
  RTT / RTO        RTT events (sample, SRTT, RTO) and the RTO in force at every
                   SEND/RETX, from the sender log
"""
import csv
import glob
import json
import math
import os
import statistics as st

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "results")
OUT = os.path.join(RES, "summary")
PLOTS = os.path.join(RES, "plots")
HDR = 12                                   # header bytes of every packet


# ================================================================ parsing
def read_log(path):
    """Return (metadata dict from '#' lines, list of row dicts)."""
    meta, lines = {}, []
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                for kv in line[1:].split():
                    if "=" in kv:
                        k, v = kv.split("=", 1)
                        meta[k] = v
            else:
                lines.append(line)
    return meta, list(csv.DictReader(lines))


def num(x, default=None):
    return int(x) if x not in (None, "") else default


# ========================================================== per-run metrics
def analyze_run(d):
    run = json.load(open(os.path.join(d, "run.json")))
    smeta, srows = read_log(os.path.join(d, "sender.log"))
    _, erows = read_log(os.path.join(d, "emulator.log"))
    ep, emu = run["endpoint"], run["emulator"]

    m = dict(exp=run["exp"], name=run["name"], proto=ep["proto"], W=ep["W"], k=ep["k"],
             seed=run["seed"], intact=run["intact"],
             data_loss=emu["data_loss"], ack_loss=emu["ack_loss"], corrupt=emu["corrupt"],
             dup=emu["dup"], reorder=emu["reorder"], delay_ms=emu["delay_ms"],
             jitter_ms=emu["jitter_ms"], rate_bps=emu["rate_bps"], payload=ep["payload"],
             file_bytes=int(smeta.get("file_bytes", 0)), packets=int(smeta.get("packets", 0)))

    sends = [r for r in srows if r["event"] in ("SEND", "RETX")]
    done = [r for r in srows if r["event"] == "DATA_DONE"]
    t0 = num(sends[0]["time_us"]) if sends else None
    t1 = num(done[0]["time_us"]) if done else None
    m["completed"] = t1 is not None
    ct = (t1 - t0) if (t0 is not None and t1 is not None) else None
    m["completion_s"] = ct / 1e6 if ct else None
    m["goodput_bps"] = m["file_bytes"] * 8 / (ct / 1e6) if ct else None

    new_b = sum(num(r["bytes"]) + HDR for r in sends if r["event"] == "SEND")
    re_b = sum(num(r["bytes"]) + HDR for r in sends if r["event"] == "RETX")
    m["tx_packets"] = len(sends)
    m["retx_packets"] = sum(r["event"] == "RETX" for r in sends)
    m["retx_overhead"] = re_b / (new_b + re_b) if sends else None
    m["timeouts"] = sum(r["event"] == "TIMEOUT" for r in srows)
    m["dup_acks"] = sum(r["event"] == "ACK_DUP" for r in srows)

    # ---- link utilization (forward link, new data only)
    data_rx = [r for r in erows if r["event"] == "RX" and r["dir"] == "fwd" and r["type"] == "DATA"]
    if len(data_rx) != len(sends):
        m["match_warning"] = f"emulator saw {len(data_rx)} DATA, sender sent {len(sends)}"
    new_pids = {rx["pid"] for rx, s in zip(data_rx, sends) if s["event"] == "SEND"}
    busy = 0
    if ct:
        for r in erows:
            if r["event"] == "ENQ" and r["dir"] == "fwd" and r["pid"] in new_pids:
                a = max(num(r["tx_start_us"]), t0)
                b = min(num(r["tx_end_us"]), t1)
                busy += max(0, b - a)
    m["utilization"] = busy / ct if ct else None

    # ---- RTT / RTO
    rtt = [r for r in srows if r["event"] == "RTT"]
    m["rtt_samples"] = len(rtt)
    m["rtt_mean_ms"] = st.mean(num(r["rtt_us"]) for r in rtt) / 1e3 if rtt else None
    m["rtt_median_ms"] = st.median(num(r["rtt_us"]) for r in rtt) / 1e3 if rtt else None
    # RTO in force when a timer expired = logged (backed-off) value / 2
    to = [num(r["rto_us"]) / 2 for r in srows if r["event"] == "TIMEOUT"]
    m["rto_at_timeout_ms"] = st.median(to) / 1e3 if to else None
    m["rto_final_ms"] = num(rtt[-1]["rto_us"]) / 1e3 if rtt else None

    # ---- emulator counters (p_eff = realised data loss rate of this run)
    m["emu_data_rx"] = len(data_rx)
    for ev in ("DROP_LOSS", "DROP_ACKLOSS", "DROP_ACTION", "DROP_QUEUE", "CORRUPT", "DUP", "HOLD"):
        m["emu_" + ev.lower()] = sum(r["event"] == ev for r in erows)
    m["p_eff"] = m["emu_drop_loss"] / len(data_rx) if data_rx else 0

    series = dict(
        rtt=[(num(r["time_us"]) - t0, num(r["rtt_us"]), num(r["srtt_us"]), num(r["rto_us"]))
             for r in rtt] if t0 is not None else [],
        rto=[(num(r["time_us"]) - t0, num(r["rto_us"])) for r in sends] if t0 is not None else [])
    return m, series


# =================================================================== theory
def base_times(m):
    """Transmission times and the no-queue round-trip time of a run."""
    R = m["rate_bps"]
    Tf = (m["payload"] + HDR) * 8 / R               # data packet on the link
    Ta = HDR * 8 / R                                # ACK on the link
    D = m["delay_ms"] / 1e3
    rtt0 = Tf + Ta + 2 * D
    return Tf, Ta, rtt0


def theory_ideal(m, W=None):
    """Exp 1/3: completion time and utilization without loss.
    Packet i (0-based) is sent at  floor(i/W)*RTT0 + (i mod W)*Tf  if the window
    limits the sender (W*Tf < RTT0), else back-to-back at i*Tf. The last packet
    is ACKed RTT0 after it was sent."""
    Tf, _, rtt0 = base_times(m)
    N = m["packets"]
    W = W or m["W"]
    if m["proto"] == "sw":
        W = 1
    if W * Tf >= rtt0:
        ct = (N - 1) * Tf + rtt0
    else:
        ct = ((N - 1) // W) * rtt0 + ((N - 1) % W) * Tf + rtt0
    return dict(completion_s=ct, goodput_bps=m["file_bytes"] * 8 / ct,
                utilization=N * Tf / ct)


def theory_loss(m, p, rto_s=None):
    """Exp 2: expected completion time and retransmission overhead with data
    loss probability p (ACKs not lost). rto_s: RTO in force (default: the
    200 ms minimum).

    SW : a packet that is lost k times waits RTO*(1+2+...+2^(k-1)) before the
         successful copy, P(k) = (1-p) p^k  ->  E[extra] = RTO * p/(1-2p).
         T = N * (RTT0 + RTO * p/(1-2p));  overhead = p.
    SR : only lost packets are resent -> overhead = p. A loss at the window
         base stalls the sender from (W*Tf) until the retransmission is ACKed
         (RTO + RTT0; the queue has drained by then): stall =
         max(0, RTO + RTT0 - W*Tf), E[#losses] = N p/(1-p).
         T = (N-1) Tf + RTT0 + N p/(1-p) * stall
    GBN: the stream restarts at the lost packet one RTO later; the restarted
         window resends everything in flight. Treating the window as full,
         E[transmissions per packet] = (1 - p + pW)/(1 - p)  (Stallings), so
         overhead = 1 - 1/E. Every loss in the restarted stream costs one RTO
         (doubled if the same packet is lost again):
         T = (N-1) Tf + RTT0 + N * RTO * p/(1-2p)"""
    Tf, _, rtt0 = base_times(m)
    N, W = m["packets"], m["W"]
    rto = rto_s if rto_s else 0.2
    if m["proto"] == "sw":
        return dict(completion_s=N * (rtt0 + rto * p / (1 - 2 * p)), retx_overhead=p)
    if m["proto"] == "sr":
        stall = max(0.0, rto + rtt0 - W * Tf)
        return dict(completion_s=(N - 1) * Tf + rtt0 + N * p / (1 - p) * stall,
                    retx_overhead=p)
    e_tx = (1 - p + p * W) / (1 - p)
    return dict(completion_s=(N - 1) * Tf + rtt0 + N * rto * p / (1 - 2 * p),
                retx_overhead=1 - 1 / e_tx)


def seq_limit(proto, k):
    """Largest safe window: GBN 2^k - 1, SR 2^(k-1)."""
    return 2 ** k - 1 if proto == "gbn" else 2 ** (k - 1)


# ================================================================== output
def save_csv(rows, path):
    keys = []
    for r in rows:
        for k in r:
            if k not in keys:
                keys.append(k)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)


def fmt(x, nd=3):
    if x is None:
        return "-"
    if isinstance(x, bool):
        return "yes" if x else "**NO**"
    if isinstance(x, float):
        return f"{x:.{nd}f}"
    return str(x)


def md_table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    out += ["| " + " | ".join(fmt(c) for c in r) + " |" for r in rows]
    return "\n".join(out) + "\n"


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def savefig(fig, name):
    fig.tight_layout()
    fig.savefig(os.path.join(PLOTS, name), dpi=130)
    plt.close(fig)


COLORS = {"sw": "#4C72B0", "gbn": "#DD8452", "sr": "#55A868"}
NAMES = {"sw": "Stop-and-Wait", "gbn": "Go-Back-N", "sr": "Selective Repeat"}


# ============================================================ experiments
def exp1(runs, md):
    rows = []
    for m, _ in runs:
        th = theory_ideal(m)
        m.update(th_completion_s=th["completion_s"], th_goodput_bps=th["goodput_bps"],
                 th_utilization=th["utilization"])
        rows.append([NAMES[m["proto"]], m["W"], m["completion_s"], m["th_completion_s"],
                     m["goodput_bps"] / 1e3, m["th_goodput_bps"] / 1e3,
                     m["utilization"], m["th_utilization"], m["retx_packets"], m["intact"]])
    md.append("## Experiment 1 - ideal network\n\n" + md_table(
        ["protocol", "W", "T meas [s]", "T theory [s]", "goodput meas [kbit/s]",
         "goodput theory [kbit/s]", "U meas", "U theory", "retx", "intact"], rows))

    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    Ws = [1, 2, 4, 8, 16, 32]
    ref = next(m for m, _ in runs if m["proto"] != "sw")
    Wfine = list(range(1, 33))
    ax[0].plot(Wfine, [theory_ideal(ref, W)["goodput_bps"] / 1e3 for W in Wfine], "k--",
               label="theory")
    ax[1].plot(Wfine, [theory_ideal(ref, W)["utilization"] for W in Wfine], "k--",
               label="theory")
    for proto, mk in (("gbn", "o"), ("sr", "s")):
        ms = sorted((m for m, _ in runs if m["proto"] == proto), key=lambda m: m["W"])
        ax[0].plot([m["W"] for m in ms], [m["goodput_bps"] / 1e3 for m in ms], mk,
                   color=COLORS[proto], label=NAMES[proto], ms=7, alpha=.8)
        ax[1].plot([m["W"] for m in ms], [m["utilization"] for m in ms], mk,
                   color=COLORS[proto], label=NAMES[proto], ms=7, alpha=.8)
    sw = next(m for m, _ in runs if m["proto"] == "sw")
    ax[0].plot([1], [sw["goodput_bps"] / 1e3], "^", color=COLORS["sw"], ms=9, label="Stop-and-Wait")
    ax[1].plot([1], [sw["utilization"]], "^", color=COLORS["sw"], ms=9, label="Stop-and-Wait")
    _, _, rtt0 = base_times(ref)
    Tf = base_times(ref)[0]
    for a in ax:
        a.axvline(rtt0 / Tf, color="grey", lw=.8, ls=":")
        a.set_xscale("log", base=2)
        a.set_xticks(Ws)
        a.set_xticklabels(Ws)
        a.set_xlabel("window size W")
        a.legend()
        a.grid(alpha=.3)
    ax[0].set_ylabel("goodput [kbit/s]")
    ax[1].set_ylabel("link utilization")
    ax[0].set_title("Exp 1: goodput vs window (dotted: W = RTT0/Tf)")
    ax[1].set_title("Exp 1: utilization vs window")
    savefig(fig, "exp1_window.png")


def exp2(runs, md):
    groups = {}
    for m, _ in runs:
        groups.setdefault((m["proto"], m["data_loss"]), []).append(m)
    rows = []
    for (proto, p), ms in sorted(groups.items()):
        rto = mean(m["rto_at_timeout_ms"] for m in ms)
        rto_s = rto / 1e3 if rto else None
        th = theory_loss(ms[0], p, rto_s)                 # nominal p
        for m in ms:
            te = theory_loss(m, m["p_eff"], rto_s)        # realised loss rate
            m.update(th_completion_s=th["completion_s"], th_retx_overhead=th["retx_overhead"],
                     th_eff_completion_s=te["completion_s"],
                     th_eff_retx_overhead=te["retx_overhead"])
        rows.append([NAMES[proto], int(p * 100), len(ms), mean(m["p_eff"] for m in ms),
                     mean(m["completion_s"] for m in ms), th["completion_s"],
                     mean(m["th_eff_completion_s"] for m in ms),
                     mean(m["retx_overhead"] for m in ms), th["retx_overhead"],
                     mean(m["th_eff_retx_overhead"] for m in ms),
                     mean(m["goodput_bps"] for m in ms) / 1e3,
                     mean(m["timeouts"] for m in ms), rto, all(m["intact"] for m in ms)])
    md.append("## Experiment 2 - data packet loss (mean over seeds)\n\n"
              "theory(p) uses the configured loss rate, theory(p&#770;) the loss rate the "
              "emulator actually realised in each run (DROP_LOSS / DATA arrivals).\n\n" + md_table(
        ["protocol", "loss %", "runs", "p&#770;", "T meas [s]", "T theory(p)", "T theory(p&#770;)",
         "overhead meas", "overhead theory(p)", "overhead theory(p&#770;)", "goodput [kbit/s]",
         "timeouts", "RTO used [ms]", "intact"], rows))

    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    for proto in ("sw", "gbn", "sr"):
        ps = sorted({p for (q, p) in groups if q == proto})
        for i, (key, thkey) in enumerate((("completion_s", "th_completion_s"),
                                          ("retx_overhead", "th_retx_overhead"))):
            meas = [mean(m[key] for m in groups[(proto, p)]) for p in ps]
            lo = [min(m[key] for m in groups[(proto, p)]) for p in ps]
            hi = [max(m[key] for m in groups[(proto, p)]) for p in ps]
            th = [groups[(proto, p)][0][thkey] for p in ps]
            x = [p * 100 for p in ps]
            ax[i].errorbar(x, meas, yerr=[[a - b for a, b in zip(meas, lo)],
                                          [b - a for a, b in zip(meas, hi)]],
                           fmt="o-", color=COLORS[proto], label=NAMES[proto] + " measured",
                           capsize=3)
            ax[i].plot(x, th, "--", color=COLORS[proto], label=NAMES[proto] + " theory")
    ax[0].set_ylabel("completion time [s]")
    ax[1].set_ylabel("retransmitted / transmitted bytes")
    for a in ax:
        a.set_xlabel("data packet loss [%]")
        a.grid(alpha=.3)
        a.legend(fontsize=8)
    ax[0].set_title("Exp 2: completion time (bars: min-max over seeds)")
    ax[1].set_title("Exp 2: retransmission overhead")
    savefig(fig, "exp2_loss.png")


def exp3(runs, md):
    rows = []
    for m, _ in runs:
        th = theory_ideal(m)
        m.update(th_completion_s=th["completion_s"], th_utilization=th["utilization"])
        rows.append([NAMES[m["proto"]], m["W"] if m["proto"] != "sw" else 1, m["delay_ms"],
                     base_times(m)[2] * 1e3, m["rtt_mean_ms"], m["rto_final_ms"],
                     m["completion_s"], m["th_completion_s"], m["utilization"],
                     m["th_utilization"], m["timeouts"], m["intact"]])
    rows.sort(key=lambda r: (r[0], r[1], r[2]))
    md.append("## Experiment 3 - propagation delay (jitter = 10 % of delay)\n\n" + md_table(
        ["protocol", "W", "delay [ms]", "RTT0 theory [ms]", "RTT mean meas [ms]",
         "final RTO [ms]", "T meas [s]", "T theory [s]", "U meas", "U theory",
         "timeouts", "intact"], rows))

    fig, ax = plt.subplots(figsize=(6.5, 4))
    ref = runs[0][0]
    dfine = list(range(10, 210, 5))
    for proto, W, mk in (("sw", 1, "^"), ("sr", 16, "s"), ("sr", 64, "D")):
        ms = sorted((m for m, _ in runs if m["proto"] == proto and (proto == "sw" or m["W"] == W)),
                    key=lambda m: m["delay_ms"])
        lbl = NAMES[proto] + ("" if proto == "sw" else f" W={W}")
        col = COLORS[proto] if W != 64 else "#8172B3"
        ax.plot([m["delay_ms"] for m in ms], [m["utilization"] for m in ms], mk, color=col,
                ms=8, label=lbl + " measured")
        th = [theory_ideal(dict(ref, delay_ms=d, proto=proto, W=W))["utilization"] for d in dfine]
        ax.plot(dfine, th, "--", color=col, label=lbl + " theory")
    ax.set_xlabel("one-way propagation delay [ms]")
    ax.set_ylabel("link utilization")
    ax.set_title("Exp 3: utilization vs propagation delay")
    ax.grid(alpha=.3)
    ax.legend(fontsize=8)
    savefig(fig, "exp3_utilization.png")

    # RTT samples and RTO over time, one panel per configuration at each delay
    cfgs = [("sw", 1), ("sr", 16), ("sr", 64)]
    fig, axs = plt.subplots(len(cfgs), 4, figsize=(16, 9))
    for i, (proto, W) in enumerate(cfgs):
        for j, delay in enumerate((20, 50, 100, 200)):
            a = axs[i][j]
            hit = [(m, s) for m, s in runs if m["proto"] == proto and m["delay_ms"] == delay
                   and (proto == "sw" or m["W"] == W)]
            if not hit:
                continue
            m, s = hit[0]
            if s["rtt"]:
                t = [x[0] / 1e6 for x in s["rtt"]]
                a.plot(t, [x[1] / 1e3 for x in s["rtt"]], ".", ms=3, label="RTT sample")
                a.plot(t, [x[2] / 1e3 for x in s["rtt"]], "-", lw=1, label="SRTT")
            if s["rto"]:
                a.plot([x[0] / 1e6 for x in s["rto"]], [x[1] / 1e3 for x in s["rto"]], "-",
                       lw=1.2, color="red", label="RTO in force")
            a.axhline(base_times(m)[2] * 1e3, color="grey", ls=":", lw=1, label="RTT0")
            # scale to the RTT samples; RTO spikes from backoff may be clipped
            top = max([x[1] / 1e3 for x in s["rtt"]] + [base_times(m)[2] * 1e3])
            a.set_ylim(0, top * 1.8)
            a.set_title(f"{NAMES[proto]}{'' if proto == 'sw' else f' W={W}'}, d={delay} ms",
                        fontsize=9)
            a.grid(alpha=.3)
            if i == len(cfgs) - 1:
                a.set_xlabel("time since first packet [s]")
            if j == 0:
                a.set_ylabel("ms")
    axs[0][0].legend(fontsize=7)
    savefig(fig, "exp3_rtt_rto.png")


def exp4(runs, md):
    groups = {}
    for m, _ in runs:
        imp = m["name"].rsplit("_", 2)[0]               # corrupt / reorder / dup / ack_loss
        m["impairment"] = imp
        groups.setdefault((imp, m["proto"]), []).append(m)
    rows = []
    for (imp, proto), ms in sorted(groups.items()):
        rows.append([imp, NAMES[proto], len(ms), all(m["intact"] for m in ms),
                     mean(m["completion_s"] for m in ms), mean(m["retx_overhead"] for m in ms),
                     mean(m["timeouts"] for m in ms), mean(m["dup_acks"] for m in ms),
                     mean(m["goodput_bps"] for m in ms) / 1e3])
    md.append("## Experiment 4 - corruption, reordering, duplication, ACK loss at 5 % "
              "(mean over seeds)\n\n" + md_table(
                  ["impairment", "protocol", "runs", "all intact", "T [s]", "overhead",
                   "timeouts", "dup ACKs", "goodput [kbit/s]"], rows))

    imps = ["corrupt", "reorder", "dup", "ack_loss"]
    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    width = .27
    for i, proto in enumerate(("sw", "gbn", "sr")):
        xs = [x + (i - 1) * width for x in range(len(imps))]
        ax[0].bar(xs, [mean(m["completion_s"] for m in groups[(imp, proto)]) for imp in imps],
                  width, color=COLORS[proto], label=NAMES[proto])
        ax[1].bar(xs, [mean(m["retx_overhead"] for m in groups[(imp, proto)]) for imp in imps],
                  width, color=COLORS[proto], label=NAMES[proto])
    for a in ax:
        a.set_xticks(range(len(imps)))
        a.set_xticklabels(imps)
        a.grid(alpha=.3, axis="y")
        a.legend()
    ax[0].set_ylabel("completion time [s]")
    ax[1].set_ylabel("retransmission overhead")
    ax[0].set_title("Exp 4: completion time (5 % impairment)")
    ax[1].set_title("Exp 4: retransmission overhead")
    savefig(fig, "exp4_impairments.png")


def exp5(runs, md):
    rows = []
    for m, _ in sorted(runs, key=lambda x: (x[0]["proto"], x[0]["k"], x[0]["W"])):
        lim = seq_limit(m["proto"], m["k"])
        expect = m["W"] <= lim
        rows.append([NAMES[m["proto"]], m["k"], m["W"], lim, "intact" if expect else "fail",
                     m["intact"], "yes" if m["intact"] == expect else "**NO**"])
    md.append("## Experiment 5 - sequence-number limits "
              "(impairment: every ACK of the first window lost)\n\n" + md_table(
                  ["protocol", "k", "W", "limit", "theory", "file intact", "matches theory"],
                  rows))

    fig, axs = plt.subplots(1, 2, figsize=(11, 3.2))
    for a, proto in zip(axs, ("gbn", "sr")):
        for row, k in enumerate((3, 4)):
            ms = sorted((m for m, _ in runs if m["proto"] == proto and m["k"] == k),
                        key=lambda m: m["W"])
            for m in ms:
                a.scatter(m["W"], k, s=120, marker="s",
                          color="#55A868" if m["intact"] else "#C44E52")
            a.axvline(seq_limit(proto, 3) + .5, color="grey", ls=":", ymax=.5)
            a.axvline(seq_limit(proto, 4) + .5, color="grey", ls=":", ymin=.5)
        a.set_yticks([3, 4])
        a.set_ylim(2.5, 4.5)
        a.set_xlabel("window size W")
        a.set_ylabel("k")
        a.set_title(f"Exp 5: {NAMES[proto]} (green intact, red SHA-256 differs; "
                    f"dotted = limit)", fontsize=9)
    savefig(fig, "exp5_limits.png")


def main():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(PLOTS, exist_ok=True)
    md = ["# Results tables (generated by experiments/analyze.py)\n"]
    handlers = {1: exp1, 2: exp2, 3: exp3, 4: exp4, 5: exp5}
    for n, fn in handlers.items():
        dirs = sorted(glob.glob(os.path.join(RES, f"exp{n}", "*", "run.json")))
        if not dirs:
            continue
        runs = [analyze_run(os.path.dirname(p)) for p in dirs]
        fn(runs, md)
        save_csv([m for m, _ in runs], os.path.join(OUT, f"exp{n}.csv"))
        warn = [m["name"] for m, _ in runs if m.get("match_warning")]
        if warn:
            md.append(f"*Note: DATA count mismatch between sender and emulator log in: "
                      f"{', '.join(warn)}*\n")
        print(f"exp{n}: {len(runs)} runs analysed")
    with open(os.path.join(OUT, "tables.md"), "w") as f:
        f.write("\n".join(md))


if __name__ == "__main__":
    main()
