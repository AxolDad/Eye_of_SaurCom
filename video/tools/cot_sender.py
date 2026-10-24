#!/usr/bin/env python3
"""Simulated ATAK team for the Cursor on Target demo: CoT events over UDP, once a second.

Usage: cot_sender.py [seconds] [host] [port] [lat] [lon]
"""
import math
import socket
import sys
import time
from datetime import datetime, timedelta, timezone

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 60
HOST = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 6969
LAT0 = float(sys.argv[4]) if len(sys.argv) > 4 else 54.6872
LON0 = float(sys.argv[5]) if len(sys.argv) > 5 else 25.2797

TEAM = [("ANDROID-ZOR1", "ZOR-1 Lead", "a-f-G-U-C", 0.004, 0.006),
        ("ANDROID-ZOR2", "ZOR-2 Medic", "a-f-G-U-C-I", -0.003, 0.004),
        ("WINTAK-OPS", "ZOR Ops", "a-f-G-E-S", 0.000, -0.008),
        ("ANDROID-ZOR3", "ZOR-3 Relay", "a-f-G-U-C", 0.007, -0.002)]


def iso(t):
    return t.strftime("%Y-%m-%dT%H:%M:%S.") + f"{t.microsecond // 1000:03d}Z"


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    start = time.monotonic()
    n = 0
    while time.monotonic() - start < SECS:
        now = datetime.now(timezone.utc)
        for k, (uid, call, typ, dlat, dlon) in enumerate(TEAM):
            a = n * 0.05 + k
            lat = LAT0 + dlat + 0.0008 * math.sin(a)
            lon = LON0 + dlon + 0.0012 * math.cos(a)
            xml = (f"<event version='2.0' uid='{uid}' type='{typ}' time='{iso(now)}' start='{iso(now)}' "
                   f"stale='{iso(now + timedelta(minutes=5))}' how='m-g'><point lat='{lat:.6f}' lon='{lon:.6f}' hae='120' "
                   f"ce='10' le='10'/><detail><contact callsign='{call}'/></detail></event>")
            s.sendto(xml.encode(), (HOST, PORT))
        n += 1
        time.sleep(1)


if __name__ == "__main__":
    main()
