#!/usr/bin/env python3
"""Builds the Eye of ZorCom videos with HyperFrames from video/storyboard.json.

For each video: one voice-over WAV per scene (Piper, offline), each scene as long as its narration plus a pause, then one standalone
HyperFrames composition (video/compositions/<id>.html) with the screenshot (video/assets/shots), a slow zoom towards the scene's focus point,
highlight boxes in screenshot coordinates (1500 x 900), a step label and the caption. Then `hyperframes render -c` to video/renders/<id>.mp4.

Usage: build.py [--no-render] [--no-voice] [video-id ...]
Environment: PIPER (the piper command), PIPER_VOICE (the .onnx voice model).
"""
import html
import json
import os
import subprocess
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # video/
HF = "hyperframes@0.8.189"
PIPER = os.environ.get("PIPER", "/tmp/claude-0/tts-venv/bin/piper")
VOICE = os.environ.get("PIPER_VOICE", "/tmp/claude-0/voices/en_US-lessac-medium.onnx")
W, H = 1920, 1080
SHOT_W, SHOT_H = 1500, 900
BOX = (210, 36, 1500, 900)        # where the screenshot sits in the frame
PAUSE = 0.9                       # silence after each narration


def wav_seconds(path):
    with wave.open(path) as w:
        return w.getnframes() / float(w.getframerate())


def voice(text, path, redo):
    if redo or not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        subprocess.run([PIPER, "-m", VOICE, "-f", path, "--sentence_silence", "0.25"], input=text.encode(), check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return wav_seconds(path)


CSS = """
* { margin: 0; padding: 0; box-sizing: border-box; }
html, body { width: 1920px; height: 1080px; overflow: hidden; background: #070a0e; }
#root { position: relative; width: 1920px; height: 1080px; overflow: hidden; font-family: Inter, sans-serif; color: #e8eef5;
        background: radial-gradient(ellipse at 30% 20%, #142233 0%, #070a0e 70%); }
.full { position: absolute; inset: 0; }
.frame { position: absolute; left: 210px; top: 36px; width: 1500px; height: 900px; overflow: hidden; border-radius: 14px;
         box-shadow: 0 30px 80px rgba(0,0,0,0.6), 0 0 0 1px rgba(120,200,255,0.18); background: #000; }
.zoom { position: absolute; inset: 0; }
.zoom img { position: absolute; left: 0; top: 0; width: 1500px; height: 900px; }
.hl { position: absolute; border: 4px solid #ffb020; border-radius: 10px; box-shadow: 0 0 0 9999px rgba(0,0,0,0.38), 0 0 24px rgba(255,176,32,0.7); }
.hl2 { position: absolute; border: 4px solid #ffb020; border-radius: 10px; box-shadow: 0 0 24px rgba(255,176,32,0.7); }
.cap { position: absolute; left: 210px; right: 210px; bottom: 26px; height: 104px; display: flex; align-items: center; gap: 22px;
       padding: 0 30px; border-radius: 14px; background: rgba(10,16,24,0.92); box-shadow: 0 0 0 1px rgba(120,200,255,0.2); }
.step { flex: none; padding: 10px 18px; border-radius: 10px; background: #ffb020; color: #10161d; font-weight: 800; font-size: 26px; }
.captext { font-size: 34px; font-weight: 600; line-height: 1.2; }
.brand { position: absolute; right: 228px; top: 52px; padding: 8px 16px; border-radius: 9px; background: rgba(7,10,14,0.85);
         color: #7fd0ff; font-weight: 700; font-size: 22px; letter-spacing: 0.04em; }
.title { display: flex; flex-direction: column; align-items: center; justify-content: center; text-align: center; gap: 28px; }
.eye { width: 170px; height: 170px; border-radius: 50%; border: 10px solid #7fd0ff; display: flex; align-items: center; justify-content: center;
       box-shadow: 0 0 60px rgba(127,208,255,0.5); }
.eye div { width: 62px; height: 62px; border-radius: 50%; background: #ffb020; box-shadow: 0 0 40px rgba(255,176,32,0.9); }
.t1 { font-size: 104px; font-weight: 800; letter-spacing: -0.02em; }
.t2 { font-size: 44px; font-weight: 500; color: #b9c7d6; max-width: 1500px; }
.t3 { font-size: 34px; font-weight: 600; color: #ffb020; }
"""


def scene_html(i, sc, start, dur, vid, vo):
    sid = f"s{i}"
    parts = [f'<div id="{sid}" class="clip full" data-start="{start:.3f}" data-duration="{dur:.3f}" data-track-index="0">']
    if sc.get("shot"):
        parts.append(f'<div class="frame"><div id="{sid}z" class="zoom"><img src="assets/shots/{sc["shot"]}.jpg" alt="" />')
        for k, b in enumerate(sc.get("boxes", [])):
            cls = "hl" if k == 0 and sc.get("dim", True) else "hl2"
            parts.append(f'<div id="{sid}b{k}" class="{cls}" style="left:{b[0]}px;top:{b[1]}px;width:{b[2]}px;height:{b[3]}px;opacity:0"></div>')
        parts.append("</div></div>")
        parts.append(f'<div class="brand">EYE OF ZORCOM</div>')
        parts.append(f'<div id="{sid}c" class="cap"><div class="step">{html.escape(sc.get("step", ""))}</div>'
                     f'<div class="captext">{html.escape(sc["caption"])}</div></div>')
    else:
        parts.append(f'<div id="{sid}t" class="full title"><div class="eye"><div></div></div>'
                     f'<div class="t1">{html.escape(sc["title"])}</div>'
                     f'<div class="t2">{html.escape(sc.get("subtitle", ""))}</div>'
                     f'<div class="t3">{html.escape(sc.get("footer", ""))}</div></div>')
    parts.append("</div>")
    parts.append(f'<audio id="{sid}a" class="clip" data-start="{start + 0.25:.3f}" data-duration="{vo:.3f}" data-track-index="1" '
                 f'data-volume="1" src="assets/vo/{vid}_{i}.wav"></audio>')
    return "\n".join(parts)


def scene_js(i, sc, start, dur):
    sid = f"s{i}"
    js = [f'tl.fromTo("#{sid}", {{opacity: 0}}, {{opacity: 1, duration: 0.35, ease: "power1.out"}}, {start:.3f});']
    if sc.get("shot"):
        fx, fy, z = sc.get("focus", [750, 450, 1.06])
        js.append(f'tl.fromTo("#{sid}z", {{scale: 1, transformOrigin: "{fx}px {fy}px"}}, '
                  f'{{scale: {z}, duration: {dur - 0.1:.3f}, ease: "sine.inOut"}}, {start:.3f});')
        js.append(f'tl.fromTo("#{sid}c", {{y: 30, opacity: 0}}, {{y: 0, opacity: 1, duration: 0.5, ease: "power2.out"}}, {start + 0.25:.3f});')
        for k, _ in enumerate(sc.get("boxes", [])):
            js.append(f'tl.fromTo("#{sid}b{k}", {{opacity: 0}}, {{opacity: 1, duration: 0.4}}, {start + 0.9 + 0.5 * k:.3f});')
    else:
        js.append(f'tl.fromTo("#{sid}t", {{scale: 0.94}}, {{scale: 1, duration: {dur - 0.1:.3f}, ease: "sine.out"}}, {start:.3f});')
    return "\n".join(js)


def build(video, render, redo_voice):
    vid = video["id"]
    t = 0.0
    scenes_h, scenes_j = [], []
    for i, sc in enumerate(video["scenes"]):
        vo = voice(sc["voice"], os.path.join(ROOT, "assets", "vo", f"{vid}_{i}.wav"), redo_voice)
        d = max(vo + PAUSE, sc.get("min", 0))
        scenes_h.append(scene_html(i, sc, t, d, vid, vo))
        scenes_j.append(scene_js(i, sc, t, d))
        t += d
    total = round(t + 0.2, 3)
    doc = f"""<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=1920, height=1080" />
    <title>{html.escape(video["title"])}</title>
    <script src="https://cdn.jsdelivr.net/npm/gsap@3.14.2/dist/gsap.min.js"></script>
    <style>{CSS}</style>
  </head>
  <body>
    <div id="root" data-composition-id="{vid}" data-start="0" data-duration="{total}" data-width="{W}" data-height="{H}" data-fps="30">
{chr(10).join(scenes_h)}
    </div>
    <script>
      const tl = gsap.timeline({{ paused: true }});
{chr(10).join(scenes_j)}
      window.__timelines["{vid}"] = tl;
    </script>
  </body>
</html>
"""
    comp = os.path.join(ROOT, f"{vid}.html")
    with open(comp, "w") as f:
        f.write(doc)
    print(f"{vid}: {len(video['scenes'])} scenes, {total:.1f} s")
    if render:
        os.makedirs(os.path.join(ROOT, "renders"), exist_ok=True)
        raw = os.path.join("renders", "raw", f"{vid}.mp4")
        os.makedirs(os.path.join(ROOT, "renders", "raw"), exist_ok=True)
        subprocess.run(["npx", "-y", HF, "render", ".", "-c", os.path.basename(comp), "-o", raw, "-q", "standard", "-f", "30"], check=True, cwd=ROOT,
                       stdout=subprocess.DEVNULL)
        # the screens are mostly still: a higher CRF keeps the published file small without visible loss on text
        out = os.path.join(ROOT, "renders", f"{vid}.mp4")
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", os.path.join(ROOT, raw), "-c:v", "libx264", "-preset", "slow", "-crf", "32",
                        "-tune", "stillimage", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "96k", "-movflags", "+faststart", out], check=True)
        print(f"{vid}: {os.path.getsize(out) / 1e6:.1f} MB")


INDEX = """<!doctype html>
<html lang="en"><head><meta charset="UTF-8" /><meta name="viewport" content="width=1920, height=1080" />
<script src="https://cdn.jsdelivr.net/npm/gsap@3.14.2/dist/gsap.min.js"></script>
<style>html, body { margin: 0; width: 1920px; height: 1080px; background: #070a0e; }</style></head>
<body><div id="root" data-composition-id="index" data-start="0" data-duration="1" data-width="1920" data-height="1080"></div>
<script>window.__timelines["index"] = gsap.timeline({ paused: true });</script></body></html>
"""


def main():
    args = sys.argv[1:]
    with open(os.path.join(ROOT, "index.html"), "w") as f:   # the project root composition the CLI expects; each video renders with -c
        f.write(INDEX)
    render = "--no-render" not in args
    redo = "--revoice" in args
    want = [a for a in args if not a.startswith("--")]
    with open(os.path.join(ROOT, "storyboard.json")) as f:
        board = json.load(f)
    for v in board["videos"]:
        if not want or v["id"] in want:
            build(v, render, redo)


if __name__ == "__main__":
    main()
