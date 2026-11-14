#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A UDP relay in front of a game server that holds some of its answers back.

    udp_delay.py <server port> <listen port>[,<listen port>...] [--info S] [--challenge S]

Each listen port on 127.0.0.1 relays to 127.0.0.1:<server port> through a socket
of its own, so the server sees one client per listen port. Answers from the server
go back at once, except an "infoResponse" (held --info seconds) and a "challenge"
(held --challenge seconds). The client then sits in "connecting" for a while and
learns the server's name partway through: what a slow server across the Internet
looks like (scripts/test-native-menu-live.sh, case joinname). Runs until killed.
"""
import argparse
import heapq
import select
import socket
import time

ap = argparse.ArgumentParser()
ap.add_argument("server", type=int)
ap.add_argument("listen")
ap.add_argument("--info", type=float, default=2.0)
ap.add_argument("--challenge", type=float, default=4.0)
a = ap.parse_args()

server = ("127.0.0.1", a.server)
pairs = []   # [front socket, back socket, client address]
for port in a.listen.split(","):
    front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    front.bind(("127.0.0.1", int(port)))
    back = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    back.bind(("127.0.0.1", 0))
    pairs.append([front, back, None])
held = []    # (when, seq, socket, data, address)
seq = 0
while True:
    now = time.monotonic()
    while held and held[0][0] <= now:
        _, _, sock, data, addr = heapq.heappop(held)
        sock.sendto(data, addr)
    timeout = max(0.0, held[0][0] - now) if held else 0.5
    ready, _, _ = select.select([s for p in pairs for s in p[:2]], [], [], timeout)
    for s in ready:
        for p in pairs:
            if s is p[0]:
                data, p[2] = s.recvfrom(65536)
                p[1].sendto(data, server)
            elif s is p[1]:
                data, _ = s.recvfrom(65536)
                if p[2] is None:
                    continue
                delay = 0.0
                if data.startswith(b"\xff\xff\xff\xffinfoResponse"):
                    delay = a.info
                elif data.startswith(b"\xff\xff\xff\xffchallenge"):
                    delay = a.challenge
                seq += 1
                heapq.heappush(held, (time.monotonic() + delay, seq, p[0], data, p[2]))
