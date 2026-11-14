#!/usr/bin/env python3
"""A fake DarkPlaces master and N fake game servers on loopback, for the
server list tests (scripts/test-srvlist.sh).

Every server has its own loopback address (127.77.x.y:26000) unless it shares
a host with others (--shared K puts the last K servers on one address, ports
26000..), a latency, and is alive or dead. Alive servers answer getstatus /
getinfo after their latency, lose a share of the answers (--loss), and, like a
stock server, ignore a second query from the same address within 1 s. The
master answers getservers / getserversExt with every server, dead ones too,
after --master-latency.

State is read from a JSON file that the test rewrites between runs (so one
process serves a whole scenario) and counters are written back to --report on
SIGUSR1 and on exit:
  queries, answers, dropped_flood (queries a stock server would have ignored),
  max_per_second (most queries in any 1 s window), min_gap_same_server,
  min_gap_same_host, max_host_burst (most queries to one IP address from one
  client within 100 ms), first_query, first_answer (seconds since the master
  was first asked), master_queries.
The state may carry master_delay: seconds the master waits before answering.
"""
import argparse, heapq, json, os, random, select, signal, socket, struct, sys, time

EOT = b"\\EOT\0\0\0"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", required=True, help="JSON: {servers: [{ip, port, alive, latency_ms, humans, bots, max, map, name}], hold_answers: seconds}")
    ap.add_argument("--report", required=True)
    ap.add_argument("--master-port", type=int, default=27950)
    ap.add_argument("--master-latency", type=float, default=0.03)
    ap.add_argument("--loss", type=float, default=0.0)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rnd = random.Random(a.seed)

    master = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    master.bind(("127.0.0.1", a.master_port))
    socks = {}       # (ip, port) -> socket
    by_fd = {}
    state = {"servers": [], "hold_answers": 0}
    state_mtime = 0
    stats = {"queries": 0, "answers": 0, "dropped_flood": 0, "max_per_second": 0, "min_gap_same_server": None,
             "min_gap_same_host": None, "max_host_burst": 0, "first_query": None, "first_answer": None, "master_queries": 0, "lost": 0}
    last_query = {}  # (server, client) -> time
    last_host = {}   # (ip, client ip) -> time
    host_window = {} # (ip, client) -> query times within the last 100 ms
    window = []      # query times within the last second
    t_master = [None]
    pending = []     # heap of (due, n, sock, data, addr, is_answer)
    counter = [0]

    def load_state():
        nonlocal state, state_mtime
        try:
            m = os.stat(a.state).st_mtime_ns
            if m == state_mtime:
                return
            with open(a.state) as f:
                state = json.load(f)
            state_mtime = m
        except (OSError, ValueError):
            return
        for s in state["servers"]:
            key = (s["ip"], s["port"])
            if key not in socks:
                so = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                so.bind(key)
                socks[key] = so
                by_fd[so.fileno()] = (so, key)
        # a reload starts a new measurement
        for k in ("queries", "answers", "dropped_flood", "max_per_second", "max_host_burst", "master_queries", "lost"):
            stats[k] = 0
        for k in ("min_gap_same_server", "min_gap_same_host", "first_query", "first_answer"):
            stats[k] = None
        stats["flood_examples"] = []
        last_query.clear(); last_host.clear(); host_window.clear(); window.clear(); t_master[0] = None

    def report(*_):
        tmp = a.report + ".tmp"
        with open(tmp, "w") as f:
            json.dump(stats, f)
        os.replace(tmp, a.report)

    signal.signal(signal.SIGUSR1, report)
    signal.signal(signal.SIGTERM, lambda *_: (report(), sys.exit(0)))
    load_state()
    report()

    def send_later(delay, sock, data, addr, is_answer=False):
        counter[0] += 1
        heapq.heappush(pending, (time.monotonic() + delay, counter[0], sock, data, addr, is_answer))

    def server_reply(key, data, addr, now):
        s = next((x for x in state["servers"] if (x["ip"], x["port"]) == key), None)
        if not s:
            return
        kind = b"statusResponse" if data.startswith(b"\xff\xff\xff\xffgetstatus") else b"infoResponse"
        challenge = data[4:].split(b" ", 1)[1] if b" " in data[4:] else b""
        stats["queries"] += 1
        if t_master[0] is not None and stats["first_query"] is None:
            stats["first_query"] = round(now - t_master[0], 4)
        window.append(now)
        while window and window[0] < now - 1.0:
            window.pop(0)
        stats["max_per_second"] = max(stats["max_per_second"], len(window))
        hw = host_window.setdefault((key[0], addr), [])
        hw.append(now)
        while hw and hw[0] < now - 0.1:
            hw.pop(0)
        stats["max_host_burst"] = max(stats["max_host_burst"], len(hw))
        q = (key, addr)
        if q in last_query:
            gap = now - last_query[q]
            if stats["min_gap_same_server"] is None or gap < stats["min_gap_same_server"]:
                stats["min_gap_same_server"] = round(gap, 4)
            if gap < 1.0:
                stats["dropped_flood"] += 1   # a stock server ignores it (net_getstatusfloodblockingtimeout)
                if len(stats.setdefault("flood_examples", [])) < 5:
                    stats["flood_examples"].append("%s:%d from %s:%d after %.3f s" % (key[0], key[1], addr[0], addr[1], gap))
                last_query[q] = now
                return
        last_query[q] = now
        h = (key[0], addr[0], addr[1])
        if h in last_host and last_host[h][1] != key:
            gap = now - last_host[h][0]
            if stats["min_gap_same_host"] is None or gap < stats["min_gap_same_host"]:
                stats["min_gap_same_host"] = round(gap, 4)
        last_host[h] = (now, key)
        if not s.get("alive", True):
            return
        if rnd.random() < a.loss:
            stats["lost"] += 1
            return
        humans, bots = s.get("humans", 0), s.get("bots", 0)
        info = (b"\\gamename\\Xonotic\\modname\\data\\gameversion\\806\\sv_maxclients\\%d\\clients\\%d\\bots\\%d"
                b"\\mapname\\%s\\hostname\\%s\\protocol\\3\\qcstatus\\dm:git:P0:S%d:F5:MXonotic::score!!\\challenge\\%s"
                % (s.get("max", 16), humans + bots, bots, s.get("map", "stormkeep").encode(), s.get("name", "Fake").encode(),
                   s.get("max", 16), challenge))
        body = b"\xff\xff\xff\xff" + kind + b"\n" + info
        if kind == b"statusResponse":
            body += b"\n" + b"".join(b'%d %d "player%d"\n' % (i, 40 + i, i) for i in range(humans))
        send_later(s.get("latency_ms", 50) / 1000.0 + state.get("hold_answers", 0), socks[key], body, addr, True)

    def master_reply(data, addr, now):
        ext = data.startswith(b"\xff\xff\xff\xffgetserversExt")
        stats["master_queries"] += 1
        if t_master[0] is None:
            t_master[0] = now
        head = b"\xff\xff\xff\xffgetserversExtResponse" if ext else b"\xff\xff\xff\xffgetserversResponse"
        entries = [b"\\" + socket.inet_aton(s["ip"]) + struct.pack(">H", s["port"]) for s in state["servers"]]
        # a real master sends about 100 addresses a packet
        for i in range(0, max(1, len(entries)), 100):
            chunk = b"".join(entries[i:i + 100])
            last = i + 100 >= len(entries)
            send_later(a.master_latency + state.get("master_delay", 0), master, head + chunk + (EOT if last else b""), addr)

    while True:
        load_state()
        now = time.monotonic()
        timeout = 0.05
        while pending and pending[0][0] <= now:
            _, _, sock, data, addr, is_answer = heapq.heappop(pending)
            try:
                sock.sendto(data, addr)
            except OSError:
                pass
            if is_answer:
                stats["answers"] += 1
                if t_master[0] is not None and stats["first_answer"] is None:
                    stats["first_answer"] = round(now - t_master[0], 4)
        if pending:
            timeout = max(0.0, min(timeout, pending[0][0] - now))
        r, _, _ = select.select([master] + list(socks.values()), [], [], timeout)
        now = time.monotonic()
        for so in r:
            try:
                data, addr = so.recvfrom(2048)
            except OSError:
                continue
            if so is master:
                if data.startswith(b"\xff\xff\xff\xffgetservers"):
                    master_reply(data, addr, now)
            elif data.startswith(b"\xff\xff\xff\xffgetstatus") or data.startswith(b"\xff\xff\xff\xffgetinfo"):
                server_reply(by_fd[so.fileno()][1], data, addr, now)


if __name__ == "__main__":
    main()
