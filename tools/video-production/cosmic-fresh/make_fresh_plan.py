"""Render plan for the new first 1:04 of Cosmic Rain (Adi 2026-11-09): new animation from the existing approved stills.

usage: make_fresh_plan.py <old root> <new root> [takes, default 3]
Reads Draft 1's plan.json for the stills, timing on bar lines, symmetry and push of every slot before frame 1531
(bar 38), and writes <new root>/plan.json and picks.json. Each slot gets one simple action that starts on the first
frame, no forced-motion wording and no motion gate: takes are judged by eye on "did the asked action happen, from the
first frame, with the same characters". Take n of every slot is rendered before take n+1, so a whole pass can be
previewed early. A bar B continues from the bar A of its own take.
"""
import json, sys
from pathlib import Path

OLD, NEW = Path(sys.argv[1]), Path(sys.argv[2])
TAKES = int(sys.argv[3]) if len(sys.argv) > 3 else 3
END = 1531
ACTIONS = {
    "K001": "Silver rain streams steadily down into the golden ring.",
    "K002": "The silver raindrops fall steadily past the camera.",
    "K003": "Her silver hair drifts slowly around her as she floats.",
    "K004": "Soft lightning flickers inside the star clouds as they swell.",
    "K005": "The giant raindrop falls straight down toward the ocean planet.",
    "K006": "The crown of water rises and rings of light spread outward.",
    "K007": "The ripples expand steadily outward.",
    "K008": "Raindrops land on the surface above and the light rays sway.",
    "K009": "The jellyfish pulse and rise together.",
    "K010": "Particles of light drift upward through the beam in front of the temple.",
    "K011": "Bubbles rise past her face and her hair waves gently.",
    "K012": "The spiral of raindrops turns around the star.",
    "TS01": "Her silver hair and the rain move around her.",
    "K013": "The elders bring their hands down on the drums together.",
    "K014": "The hands come down and strike the glowing drum.",
    "K015a": "Everyone strikes their drums together again and again.",
    "K015b": "Everyone keeps striking their drums together.",
    "K016": "The glowing clock hands sweep around the clock face.",
    "K017": "They keep drumming together, steady and calm.",
    "K018": "Sparks of light race along the neurons.",
    "K019a": "The neuron lights stretch into rain and pour down.",
    "K019b": "The silver rain keeps pouring down toward the ocean.",
    "K020": "The clock hands spin very fast.",
}
old = json.loads((OLD / "plan.json").read_text(encoding="utf-8"))
oldpicks = json.loads((OLD / "picks.json").read_text(encoding="utf-8"))
base, picks = [], {}
for s in old["slots"]:
    if s["frames"][0] >= END:
        break
    key = s.get("key")
    if s.get("reused"):
        gen = Path(s["take_root"]) / "anim" / s["take"] / "generated.json" if s.get("take_root") else None
        assert gen and gen.exists(), s
        key = "ts01"
        picks[key] = str(Path(s["take_root"]) / json.loads(gen.read_text())["keyframe"])
    else:
        picks[key] = str(OLD / "raw" / oldpicks[key])
    assert Path(picks[key]).exists(), picks[key]
    new = {k: s[k] for k in ("frames", "length", "step", "composite", "push", "portal_in", "dissolve_in") if k in s}
    base.append({**new, "id": s["id"], "scene": s["id"], "key": key, "prompt": ACTIONS[s["id"]],
                 "chain_from": s.get("chain_from")})
assert {b["id"] for b in base} == set(ACTIONS), {b["id"] for b in base} ^ set(ACTIONS)
slots = []
for t in range(1, TAKES + 1):
    for i, b in enumerate(base):
        s = {**b, "id": f"{b['id']}-{t}", "slot": b["id"], "take_no": t, "seed": 161100000 + 1000 * t + i}
        if b["chain_from"]:
            s["chain_from"] = f"{b['chain_from']}-{t}"
        else:
            s.pop("chain_from")
        slots.append(s)
plan = {"title": "Cosmic Rain - new first 1:04", "fps": old["fps"], "style": old["style"], "opening": old["opening"],
        "audio": old["audio"], "end_frame": END, "slots": slots}
(NEW / "plan.json").write_text(json.dumps(plan, indent=1), encoding="utf-8")
(NEW / "picks.json").write_text(json.dumps(picks, indent=1), encoding="utf-8")
per = sum(b["length"] for b in base)
print(len(base), "slots x", TAKES, "takes;", per, "generated frames per pass, about", round(per / 81 * 675 / 3600, 1), "h per pass")
