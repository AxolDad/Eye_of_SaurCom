#!/usr/bin/env python3
"""Screenshots of the real program for the videos: each shot starts dect2 off screen (Xvfb) with preset settings and dev flags, optionally
runs a simulated sender next to it, and keeps the window as video/assets/shots/<name>.jpg (the program's own --shot, 1500 x 900).

Usage: capture.py [name ...]   (no names: every shot)
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, "build", "dect2")
OUT = os.path.join(ROOT, "video", "assets", "shots")
TOOLS = os.path.join(ROOT, "video", "tools")
CONF = os.path.expanduser("~/.config/saurcom/settings.conf")

SCAN = "\t".join([
    "frequency_mhz,mode,name,dwell_s,lockout,priority,trunk",
    "851.012500,p25,County P25,8.00,0,0,1",
    "460.500000,nxdn,City NXDN,8.00,0,0,1",
    "453.250000,dmr,Port DMR,8.00,0,0,1",
    "146.520000,fm,2 m calling,2.00,0,1,0",
    "121.500000,airband,Guard,2.00,0,0,0",
])

BASE = {
    "tourDone": "1", "firstRun": "0", "wizardDone": "1", "mapDeclined": "1", "mapDeclinedVersion": "0.2.9",
    "scanList": SCAN,
    "trunkDmrChannels": "1=453.2500 2=453.2750 3=453.3000",
    "trunkNxdnChannels": "1=460.5000 2=460.5125 513=461.0125",
}

# name: (args, delay s, extra settings, sender [script, args])
SHOTS = {
    "main_fm": (["--fm", "--autostart"], 8, {}, None),
    "main_dab": (["--dab", "--autostart"], 10, {}, None),
    "adsb_map": (["--mode", "adsb", "--autostart", "--map"], 14, {}, None),
    "tx_mesh": (["--tx", "mesh"], 4, {}, None),
    "tx_dry": (["--txdry", "mesh"], 10, {}, None),
    "feeds": (["--mode", "adsb", "--autostart", "--feeds"], 8, {}, None),
    "cot_map": (["--mode", "adsb", "--autostart", "--cotlisten", "--map"], 12, {}, ["cot_sender.py", "30", "127.0.0.1", "6969", "25.20", "55.40"]),
    "p25": (["--mode", "p25", "--autostart"], 12, {}, None),
    "p25_follow": (["--mode", "p25", "--autostart", "--tab", "P25", "--p25view", "5"], 14, {}, None),
    "p25_trunking": (["--mode", "p25", "--autostart", "--tab", "P25", "--p25view", "2"], 14, {}, None),
    "dmr_follow": (["--mode", "dmr", "--autostart", "--tab", "DMR", "--trunkopen"], 10, {}, None),
    "dmr": (["--mode", "dmr", "--autostart"], 10, {}, None),
    "nxdn": (["--mode", "nxdn", "--autostart"], 10, {}, None),
    "nxdn_site": (["--mode", "nxdn", "--autostart", "--tab", "NXDN", "--nxdnview", "3", "--trunkopen"], 12, {}, None),
    "scanner": (["--mode", "fm", "--scanner"], 4, {}, None),
    "webradio": (["--webradio"], 4, {}, None),
    "wifisense": (["--wifisense"], 32, {}, ["csi_sender.py", "45"]),
    "mesh_rx": (["--mode", "meshtastic", "--autostart"], 14, {}, None),
}


def write_conf(extra):
    os.makedirs(os.path.dirname(CONF), exist_ok=True)
    kv = dict(BASE, **extra)
    with open(CONF, "w") as f:
        for k, v in kv.items():
            f.write(f"{k}={v}\n")


def shot(name):
    args, delay, extra, sender = SHOTS[name]
    write_conf(extra)
    raw = f"/tmp/claude-0/shots/{name}.png"
    os.makedirs(os.path.dirname(raw), exist_ok=True)
    if os.path.exists(raw):
        os.remove(raw)
    snd = subprocess.Popen([sys.executable, os.path.join(TOOLS, sender[0])] + sender[1:]) if sender else None
    env = dict(os.environ, DECT2_MUTE="1")
    cmd = ["xvfb-run", "-a", "-s", "-screen 0 1920x1080x24", APP] + args + ["--mute", "--shot", raw, "--shotdelay", str(delay)]
    try:
        subprocess.run(cmd, cwd=os.path.dirname(APP), env=env, timeout=delay + 60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        print(f"{name}: timed out")
    if snd:
        snd.terminate()
    if not os.path.exists(raw):
        print(f"{name}: no screenshot")
        return
    os.makedirs(OUT, exist_ok=True)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", raw, "-q:v", "3", os.path.join(OUT, name + ".jpg")], check=True)
    print(f"{name}: ok")


def main():
    names = sys.argv[1:] or list(SHOTS)
    if os.path.exists(CONF):
        shutil.copy(CONF, CONF + ".before-capture")
    for n in names:
        shot(n)


if __name__ == "__main__":
    main()
