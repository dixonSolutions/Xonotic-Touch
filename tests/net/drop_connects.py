#!/usr/bin/env python3
"""A fake server that ignores the first N connect requests, then accepts
(scripts/test-net-connect.sh): what a real host did to one client, leaving
one or two connects of a join unanswered.

    drop_connects.py PORT N SECONDS

Answers every getchallenge with a challenge, drops the first N connects,
answers the next with "accept", and prints one line per packet with the
time since the first getchallenge.

N = "flood" plays a stock DarkPlaces server's connect flood block instead:
another device behind the same address joined just before (a connect one
second before the first getchallenge), and every connect within
net_connectfloodblockingtimeout (5 s) of the last one is ignored and renews
the block. The stock client, asking every second, never gets in.
"""
import socket
import sys
import time

port, secs = int(sys.argv[1]), float(sys.argv[3])
flood = sys.argv[2] == "flood"
n = 0 if flood else int(sys.argv[2])
BLOCK = 5.0
last_connect = None
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", port))
s.settimeout(0.2)
end = time.time() + secs
t0 = None
dropped = 0
while time.time() < end:
    try:
        data, addr = s.recvfrom(4096)
    except OSError:
        continue
    if not data.startswith(b"\xff\xff\xff\xff"):
        continue
    now = time.time()
    if t0 is None:
        t0 = now
        last_connect = now - 1.0  # the other device's connect
    if data[4:].startswith(b"getchallenge"):
        s.sendto(b"\xff\xff\xff\xffchallenge 12345abc", addr)
        print(f"{now - t0:7.3f} getchallenge -> challenge", flush=True)
    elif data[4:].startswith(b"connect"):
        if flood and now < last_connect + BLOCK:
            last_connect = now
            dropped += 1
            print(f"{now - t0:7.3f} connect #{dropped}: blocked (flood)", flush=True)
        elif not flood and dropped < n:
            dropped += 1
            print(f"{now - t0:7.3f} connect #{dropped}: dropped", flush=True)
        else:
            s.sendto(b"\xff\xff\xff\xffaccept", addr)
            print(f"{now - t0:7.3f} connect: accepted", flush=True)
            break
