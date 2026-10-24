#!/usr/bin/env python3
"""Simulated RuView ESP32 CSI nodes for the WiFi sensing demo: ADR-018 frames over UDP.

Node 1: a person breathing 16 times a minute (heart 68 a minute), node 2: an empty room, node 3: someone walking about.
Usage: csi_sender.py [seconds] [host] [port]
"""
import math
import random
import socket
import struct
import sys
import time

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 60
HOST = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 5005
FS, NSUB = 50.0, 52


def frame(node, seq, rssi, amp):
    b = struct.pack("<IBBHIIbbBB", 0xC5110001, node, 1, NSUB, 2437, seq, rssi, -92, 0, 0)
    iq = bytearray()
    for k in range(NSUB):
        ph = 0.3 + 0.05 * k
        a = amp(k)
        iq += struct.pack("bb", max(-127, min(127, round(a * math.cos(ph)))), max(-127, min(127, round(a * math.sin(ph)))))
    return b + bytes(iq)


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rng = random.Random(7)
    start = time.monotonic()
    i = 0
    while time.monotonic() - start < SECS:
        t = i / FS
        breath = 6 * math.sin(2 * math.pi * (16 / 60) * t) + 1.5 * math.sin(2 * math.pi * (68 / 60) * t)
        walk = 14 * math.sin(2 * math.pi * 0.7 * t) * math.sin(2 * math.pi * 0.13 * t)
        s.sendto(frame(1, i, -48, lambda k: 60 + breath + rng.uniform(-0.3, 0.3)), (HOST, PORT))
        s.sendto(frame(2, i, -55, lambda k: 58 + rng.uniform(-0.3, 0.3)), (HOST, PORT))
        s.sendto(frame(3, i, -61, lambda k: 55 + walk * math.sin(0.3 * k + t) + rng.uniform(-4, 4)), (HOST, PORT))
        i += 1
        time.sleep(max(0.0, start + i / FS - time.monotonic()))


if __name__ == "__main__":
    main()
