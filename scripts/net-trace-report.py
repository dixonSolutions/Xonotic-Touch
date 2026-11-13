#!/usr/bin/env python3
"""Per-phase timeline of a join from a client log with touch_net_trace 1.

    scripts/net-trace-report.py [--rtt MS] [--json] CLIENT_LOG...

Phases (client side, docs/NETWORK.md):
  connect    getchallenge sent -> accept received (challenge + connect: 2 RTT)
  serverinfo accept -> signon 1 (serverinfo, precache lists, stufftexts)
  precache   signon 1 -> "prespawn" sent (player info, CSQC check or
             download, every precached model and sound loaded: local work)
  signon2    prespawn sent -> signon 2 (baselines and static entities)
  spawn      signon 2 -> signon 3 (client data, lightstyles, stats)
  begin      signon 3 -> signon 4 (begin, first entity frame)

For each phase: wall time, client frame work inside it (rendering the
loading screen included), the rest (waiting for the peer), that wait in
round trips when --rtt is given, packets and bytes each way, reliable data
fragments received (duplicates are the server's resends) and sent.
With several logs the median of each figure is printed.
"""
import json
import re
import statistics
import sys

LINE = re.compile(r"NETTRACE cl ([0-9.]+) (.*)$")
PHASES = ["connect", "serverinfo", "precache", "signon2", "spawn", "begin"]


def parse(path):
    events = []
    for raw in open(path, errors="replace"):
        raw = re.sub(r"\x1b\[[0-9;]*m", "", raw).rstrip()
        m = LINE.search(raw)
        if m:
            events.append((float(m.group(1)), m.group(2).split()))
    return events


def work_of(fields):
    for f in fields:
        if f.startswith("w=") or f.startswith("work="):
            return float(f.split("=", 1)[1])
    return None


def analyse(path):
    ev = parse(path)
    marks = {}
    for t, f in ev:
        if f[0] == "tx" and f[1] == "oob" and len(f) > 3 and f[3] == "getchallenge" and "start" not in marks:
            marks["start"] = (t, work_of(f) or 0.0)
        elif f[0] == "rx" and f[1] == "oob" and len(f) > 3 and f[3] == "accept" and "accept" not in marks:
            marks["accept"] = (t, work_of(f))
        elif f[0] == "signon" and f[1] in "1234" and "signon" + f[1] not in marks:
            marks["signon" + f[1]] = (t, work_of(f))
        elif f[0] == "tx" and f[1].startswith("data") and len(f) > 4 and "prespawn" in f[4].split(",") and "prespawn" not in marks:
            marks["prespawn"] = (t, work_of(f))
    bounds = ["start", "accept", "signon1", "prespawn", "signon2", "signon3", "signon4"]
    if any(b not in marks for b in bounds):
        return None
    out = {}
    for i, name in enumerate(PHASES):
        t0, w0 = marks[bounds[i]]
        t1, w1 = marks[bounds[i + 1]]
        row = {"wall": t1 - t0, "work": max(0.0, (w1 or 0) - (w0 or 0)),
               "rx_pkts": 0, "tx_pkts": 0, "rx_bytes": 0, "tx_bytes": 0,
               "rx_frag": 0, "rx_dup": 0, "tx_frag": 0, "tx_resend": 0}
        seen_rx, seen_tx = set(), set()
        for t, f in ev:
            if not (t0 <= t < t1) or f[0] not in ("rx", "tx"):
                continue
            size = int(f[2]) if f[1] == "oob" else int(f[2])
            row[f[0] + "_pkts"] += 1
            row[f[0] + "_bytes"] += size + 28
            if f[1].startswith("data"):
                seq = int(f[3])
                if f[0] == "rx":
                    row["rx_frag"] += 1
                    if seq in seen_rx:
                        row["rx_dup"] += 1
                    seen_rx.add(seq)
                else:
                    row["tx_frag"] += 1
                    if seq in seen_tx:
                        row["tx_resend"] += 1
                    seen_tx.add(seq)
        row["wait"] = max(0.0, row["wall"] - row["work"])
        out[name] = row
    tot = {k: sum(out[p][k] for p in PHASES) for k in out["connect"]}
    out["total"] = tot
    return out


def main():
    args = sys.argv[1:]
    rtt = None
    as_json = False
    while args and args[0].startswith("--"):
        if args[0] == "--rtt":
            rtt = float(args[1]); args = args[2:]
        elif args[0] == "--json":
            as_json = True; args = args[1:]
        else:
            print(__doc__); return 1
    runs = [r for r in (analyse(p) for p in args) if r]
    if not runs:
        print("no complete join in", " ".join(args), file=sys.stderr)
        return 1
    med = {}
    for ph in PHASES + ["total"]:
        med[ph] = {k: statistics.median(r[ph][k] for r in runs) for k in runs[0][ph]}
        if rtt:
            med[ph]["wait_rtt"] = med[ph]["wait"] / rtt if rtt > 0 else 0
    if as_json:
        print(json.dumps({"runs": len(runs), "phases": med}))
        return 0
    print(f"{len(runs)} run(s), medians" + (f", RTT {rtt:g} ms" if rtt else ""))
    hdr = f"{'phase':<11}{'wall ms':>9}{'work':>8}{'wait':>8}" + (f"{'wait/RTT':>9}" if rtt else "") + \
        f"{'rx pkt':>7}{'tx pkt':>7}{'rx KB':>7}{'tx KB':>7}{'rx frag':>8}{'dup':>5}{'tx frag':>8}{'resend':>7}"
    print(hdr)
    for ph in PHASES + ["total"]:
        r = med[ph]
        line = f"{ph:<11}{r['wall']:9.0f}{r['work']:8.0f}{r['wait']:8.0f}"
        if rtt:
            line += f"{r['wait_rtt']:9.1f}"
        line += f"{r['rx_pkts']:7.0f}{r['tx_pkts']:7.0f}{r['rx_bytes']/1024:7.1f}{r['tx_bytes']/1024:7.1f}" \
            f"{r['rx_frag']:8.0f}{r['rx_dup']:5.0f}{r['tx_frag']:8.0f}{r['tx_resend']:7.0f}"
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
