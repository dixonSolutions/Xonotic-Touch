#!/usr/bin/env python3
"""A hostile peer for the windowed reliable channel (scripts/test-net-fuzz.sh).

    hostile_peer.py client HOST PORT SECONDS SEED   # attack a server
    hostile_peer.py server PORT SECONDS SEED        # be a server a client joins

As a client it connects over and over, each time offering a different bogus
or extreme "rwin" window, then sends random reliable fragments (sequences in
and far outside the window, in and out of order, duplicated, with and
without end-of-message, lengths from 0 to the UDP maximum), acknowledgements
of sequences never sent, truncated and malformed headers. Now and then it
plays a fair, well-formed exchange so the deeper paths (reorder buffer, ready
queue, delivery of several messages at once) run too.

As a server it answers getchallenge and connect, with "accept\\rwin\\<bogus>"
values, then floods the client with the same kind of packets.

Prints a line per round and the counts at the end; the shell script checks
the engine is alive, quiet under AddressSanitizer and of bounded size.
"""
import random
import socket
import struct
import sys
import time

DATA, ACK, EOM, UNREL, CTL = 0x00010000, 0x00020000, 0x00080000, 0x00100000, 0x80000000
BOGUS_WINDOWS = ["32", "0", "1", "2", "-5", "abc", "", "99999999999", "4294967297", "1e9", "16\\rwin\\64",
                 "64", "65", "1000000", " 7", "0x20", "%s" % ("9" * 300)]
CLC_STRINGCMD = 4
SVC_NOP = 1


def oob(text):
    return b"\xff\xff\xff\xff" + text


def header(flags, payload_len, seq, lie=0):
    return struct.pack(">II", ((8 + payload_len + lie) & 0xFFFF) | flags, seq & 0xFFFFFFFF)


def rand_packet(rng, base, nops=False):
    """one hostile packet; base = a sequence near where the peer is. With
    nops the payloads are svc_nop bytes: the transport is attacked, not the
    game's message parser (a client's entity parser stops the engine on
    random entity data, stock DarkPlaces behaviour that this test is not
    about)"""
    kind = rng.random()
    if kind < 0.35:  # data fragment, anywhere
        seq = rng.choice([base, base + 1, base + rng.randint(2, 40), base + rng.randint(60, 5000),
                          base - rng.randint(1, 50), rng.getrandbits(32), 0xFFFFFFFF])
        n = rng.choice([0, 1, 7, 512, 1024, 1025, 1391, rng.randint(0, 1392)])
        flags = DATA | (EOM if rng.random() < 0.5 else 0)
        body = bytes([CLC_STRINGCMD]) + b"say hi\0" if rng.random() < 0.3 else rng.randbytes(n)
        if nops:
            body = bytes([SVC_NOP]) * n
        return header(flags, len(body), seq) + body
    if kind < 0.6:  # acknowledgement of anything
        seq = rng.choice([base, base - 1, base + rng.randint(1, 100), rng.getrandbits(32), 0xFFFFFFFF, 0])
        return header(ACK, 0, seq)
    if kind < 0.7:  # several flags at once, or none
        flags = rng.choice([DATA | ACK, DATA | UNREL, ACK | UNREL | EOM, 0, 0x7F000000, DATA | 0x00200000])
        body = bytes([SVC_NOP]) * rng.randint(0, 64) if nops else rng.randbytes(rng.randint(0, 64))
        return header(flags, len(body), rng.getrandbits(32)) + body
    if kind < 0.8:  # a length field that lies
        body = bytes([SVC_NOP]) * rng.randint(0, 64) if nops else rng.randbytes(rng.randint(0, 64))
        return header(DATA | EOM, len(body), base, lie=rng.choice([-3, 5, 1000])) + body
    if kind < 0.9:  # too short
        return rng.randbytes(rng.randint(0, 7))
    body = bytes([SVC_NOP]) * rng.randint(0, 200) if nops else rng.randbytes(rng.randint(0, 200))  # unreliable
    return header(UNREL, len(body), rng.getrandbits(32)) + body


def fair_burst(rng, seq, text):
    """a well-formed message in fragments, sent in a shuffled order with a
    duplicate: exercises the reorder buffer and in-order delivery"""
    msg = bytes([CLC_STRINGCMD]) + text + b"\0"
    msg = msg * rng.randint(1, 40)  # up to a few KB: several fragments
    frags = [msg[i:i + 1024] for i in range(0, len(msg), 1024)]
    pkts = []
    for i, f in enumerate(frags):
        pkts.append(header(DATA | (EOM if i == len(frags) - 1 else 0), len(f), seq + i) + f)
    order = list(range(len(pkts)))
    rng.shuffle(order)
    out = [pkts[i] for i in order]
    if out:
        out.insert(rng.randrange(len(out) + 1), rng.choice(out))
    return out, seq + len(frags)


def client_mode(host, port, seconds, seed):
    rng = random.Random(seed)
    end = time.time() + seconds
    rounds = sent = accepted = 0
    while time.time() < end:
        rounds += 1
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(0.5)
        addr = (host, port)
        try:
            s.sendto(oob(b"getchallenge"), addr)
            data, _ = s.recvfrom(2048)
        except OSError:
            s.close(); time.sleep(0.2); continue
        if not data.startswith(oob(b"challenge ")):
            s.close(); continue
        challenge = data[len(oob(b"challenge ")):].split(b"\0")[0].split(b"\\")[0]
        w = rng.choice(BOGUS_WINDOWS)
        s.sendto(oob(b"connect\\protocol\\darkplaces 3\\protocols\\DP7\\challenge\\" + challenge +
                     b"\\name\\Hostile\\rwin\\" + w.encode()), addr)
        reply = b""
        try:
            reply, _ = s.recvfrom(2048)
        except OSError:
            pass
        if reply.startswith(oob(b"accept")):
            accepted += 1
        print(f"round {rounds}: rwin {w[:20]!r} -> {reply[4:40]!r}", flush=True)
        s.setblocking(False)
        seq = 0
        for _ in range(rng.randint(200, 1500)):
            if rng.random() < 0.05:
                pkts, seq = fair_burst(rng, seq, b"echo fair")
            else:
                pkts = [rand_packet(rng, seq)]
            for p in pkts:
                try:
                    s.sendto(p, addr); sent += 1
                except OSError:
                    pass
            try:
                while True:
                    s.recvfrom(65536)
            except OSError:
                pass
        s.close()
    print(f"client mode: {rounds} rounds, {accepted} accepted, {sent} hostile packets", flush=True)


def server_mode(port, seconds, seed):
    rng = random.Random(seed)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    s.settimeout(0.05)
    end = time.time() + seconds
    peer = None
    sent = joins = 0
    seq = 0
    while time.time() < end:
        try:
            data, addr = s.recvfrom(65536)
        except OSError:
            data, addr = None, None
        if data and data.startswith(oob(b"getchallenge")):
            s.sendto(oob(b"challenge " + str(rng.getrandbits(31)).encode()), addr)
            continue
        if data and data.startswith(oob(b"connect")):
            w = rng.choice(BOGUS_WINDOWS)
            s.sendto(oob(b"accept\\rwin\\" + w.encode()), addr)
            peer = addr; seq = 0; joins += 1
            print(f"join {joins}: accept rwin {w[:20]!r}", flush=True)
            continue
        if data and data[:4] == b"\xff\xff\xff\xff":
            continue
        if peer:
            for _ in range(rng.randint(1, 30)):
                if rng.random() < 0.05:
                    body = bytes([SVC_NOP]) * rng.randint(1, 3000)
                    frags = [body[i:i + 1024] for i in range(0, len(body), 1024)]
                    pkts = [header(DATA | (EOM if i == len(frags) - 1 else 0), len(f), seq + i) + f for i, f in enumerate(frags)]
                    rng.shuffle(pkts)
                    seq += len(frags)
                else:
                    pkts = [rand_packet(rng, seq, nops=True)]
                for p in pkts:
                    s.sendto(p, peer); sent += 1
    print(f"server mode: {joins} joins, {sent} hostile packets", flush=True)


if __name__ == "__main__":
    if sys.argv[1] == "client":
        client_mode(sys.argv[2], int(sys.argv[3]), float(sys.argv[4]), int(sys.argv[5]))
    elif sys.argv[1] == "server":
        server_mode(int(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4]))
    else:
        print(__doc__)
        sys.exit(1)
