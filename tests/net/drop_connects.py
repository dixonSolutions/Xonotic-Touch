#!/usr/bin/env python3
"""A fake server that ignores the first N connect requests, then accepts
(scripts/test-net-connect.sh): what a real host did to one client, leaving
one or two connects of a join unanswered.

    drop_connects.py PORT N SECONDS

Answers every getchallenge with a challenge, drops the first N connects,
answers the next with "accept", and prints one line per packet with the
time since the first getchallenge.
"""
import socket
import sys
import time

port, n, secs = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
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
    t0 = t0 or now
    if data[4:].startswith(b"getchallenge"):
        s.sendto(b"\xff\xff\xff\xffchallenge 12345abc", addr)
        print(f"{now - t0:7.3f} getchallenge -> challenge", flush=True)
    elif data[4:].startswith(b"connect"):
        if dropped < n:
            dropped += 1
            print(f"{now - t0:7.3f} connect #{dropped}: dropped", flush=True)
        else:
            s.sendto(b"\xff\xff\xff\xffaccept", addr)
            print(f"{now - t0:7.3f} connect: accepted", flush=True)
            break
