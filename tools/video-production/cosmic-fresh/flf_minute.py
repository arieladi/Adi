"""One-minute first+last-frame test (Adi 2026-11-11): Cosmic Rain bars 30-66 (0:50-1:51), elders drumming into the
drop, the family joining, the Rain Keeper, the light ship arriving on the 1:47 drop.

usage: flf_minute.py <root>      writes <root>/stills.json and <root>/plan.json
Every shot is 2 bars and goes from a start still to a landing still (img2img of the landing, so the same people and
place). The landing - a stick hitting the drum, eyes opening, arms up - is the last usable frame, so it falls on the
next bar line, which is also the cut: the hits land on the downbeat. Clips are rendered 85 frames long; the last
latent (4 frames) of a pinned end decodes washed out, so 81 frames are kept.
"""
import json, sys
from pathlib import Path

R = Path(sys.argv[1])
BPM, PHASE, FPS, A = 143, 0.0215, 24, 30
BAR = 240 / BPM
STYLE = ("psychedelic psytrance poster illustration, visionary art, bold clean ink outlines, cel shading, vivid "
         "saturated silver, cyan, deep ultramarine and gold palette with magenta accents, glowing highlights, high "
         "contrast, intricate ornamental detail, centered composition")
E1 = ("an old tribal elder with long white hair and a long white beard, white painted stripes on his face, a feather "
      "and bead necklace, a deep blue robe")
E3 = ("three old tribal elders with long white hair and long white beards, white painted stripes on their faces, "
      "feather and bead necklaces, deep blue robes, each behind a large round glowing drum")
FAM = "women and children with painted faces and glowing bead necklaces"
G = ("the Rain Keeper goddess, pale silver-blue skin, long flowing silver hair, a golden headband with three round "
     "turquoise gems, gold teardrop earrings")
NIGHT = "night, firelight, silver rain, cosmic sky"
UP = "holding two wooden drumsticks raised high above his head, ready to strike"
UP3 = "all holding wooden drumsticks raised high above their heads, ready to strike"

# (shot, landing still prompt, start still prompt (img2img of the landing), action)
SHOTS = [
    ("M01", f"close-up of {E1} striking a large round glowing drum with two wooden drumsticks, light bursting from the drum skin, {NIGHT}",
     f"close-up of {E1} behind a large round glowing drum, {UP}, {NIGHT}", "He swings both drumsticks down and strikes the drum."),
    ("M02", f"{E3}, all striking their drums at once with wooden drumsticks, bursts of light from every drum, {NIGHT}",
     f"{E3}, {UP3}, {NIGHT}", "The three elders swing their drumsticks down together and strike the drums."),
    ("M03", f"{FAM} running in from both sides to a fire where {E3} drum, all of them clapping, {NIGHT}",
     f"{FAM} far away at the edges of a fire where {E3} drum, {NIGHT}", "The women and children run in toward the fire."),
    ("M04", f"top-down view of a large round glowing drum skin covered in raindrops, a wooden drumstick hitting its center, a ring of water and light splashing outward, {NIGHT}",
     f"top-down view of a large round glowing drum skin covered in raindrops, a wooden drumstick high above it, {NIGHT}", "The drumstick comes down and hits the center of the drum."),
    ("M05", f"close-up of the face of {E1}, his eyes wide open and glowing silver, {NIGHT}",
     f"close-up of the face of {E1}, his eyes closed, {NIGHT}", "He slowly opens his eyes and they glow."),
    ("M06", f"a child with a painted face striking a small glowing hand drum with an open palm, sparks of light, {NIGHT}",
     f"a child with a painted face holding one open hand high above a small glowing hand drum, {NIGHT}", "The child brings the hand down onto the drum."),
    ("M07", f"seen from behind, {E3} drumming around a fire, a tall column of golden sparks rising into the cosmic sky, {NIGHT}",
     f"seen from behind, {E3} drumming around a small fire, {NIGHT}", "The fire flares up and sparks rise into the sky."),
    ("M08", f"{G} high in a cosmic sky with both arms raised, silver rain pouring down from her hands",
     f"{G} high in a cosmic sky with her arms lowered at her sides, eyes closed", "She raises both arms and silver rain pours from her hands."),
    ("M09", f"wide view of {E3} striking their drums in heavy silver rain, bursts of light from every drum, {FAM} dancing behind them, {NIGHT}",
     f"wide view of {E3} in heavy silver rain, {UP3}, {FAM} dancing behind them, {NIGHT}", "The elders swing their drumsticks down and strike the drums."),
    ("M10", f"top-down view of a large round glowing drum skin with a wide ring of light rippling out to its edge, {NIGHT}",
     f"top-down view of a large round glowing drum skin with a small spot of light at its center, {NIGHT}", "A ring of light ripples out from the center to the edge."),
    ("M11", f"side view of {E1} striking a large round glowing drum with two wooden drumsticks, light bursting, {NIGHT}",
     f"side view of {E1} behind a large round glowing drum, {UP}, {NIGHT}", "He swings both drumsticks down and strikes the drum."),
    ("M12", f"{FAM} dancing with both arms raised high, glowing beads flying, {NIGHT}",
     f"{FAM} dancing with their arms low, {NIGHT}", "They dance and throw their arms up high."),
    ("M13", f"close-up of {E3}, all striking their drums with wooden drumsticks, bursts of light, {NIGHT}",
     f"close-up of {E3}, {UP3}, {NIGHT}", "The three elders swing their drumsticks down and strike the drums."),
    ("M14", f"close-up portrait of {G}, eyes wide open and glowing, silver rain falling around her face",
     f"close-up portrait of {G}, eyes closed, silver rain falling around her face", "She slowly opens her eyes."),
    ("M15", f"wide view of {E3} and {FAM} all striking drums at once, a huge burst of light and water rising from the circle, {NIGHT}",
     f"wide view of {E3} and {FAM} in a circle, all holding drumsticks raised high, {NIGHT}", "Everyone swings down and strikes the drums at once."),
    ("M16", f"the clouds in the cosmic sky parting around a glowing ship of light above a fire circle of drummers, {NIGHT}",
     f"thick glowing clouds in the cosmic sky above a fire circle of drummers, {NIGHT}", "The clouds part and a glowing ship of light appears."),
    ("M17", f"close-up of {E1} resting both hands softly on a large round glowing drum, eyes closed, gentle rain, {NIGHT}",
     f"close-up of {E1} holding both hands just above a large round glowing drum, eyes closed, gentle rain, {NIGHT}", "He lowers his hands softly onto the drum."),
    ("M18", f"a glowing alien ship of light hovering low over {E3}, beams of light pouring down onto the drums, {NIGHT}",
     f"a glowing alien ship of light high in the cosmic sky far above {E3}, {NIGHT}", "The ship of light descends and beams of light pour down."),
]


def F(bar):
    return round(FPS * (PHASE + bar * BAR))


stills, slots = [], []
for i, (sid, land, start, action) in enumerate(SHOTS):
    a, b = A + 2 * i, A + 2 * i + 2
    stills.append({"id": f"{sid}e", "prompt": land, "seed": 7100 + i})
    stills.append({"id": f"{sid}s", "from": f"{sid}e", "prompt": start, "denoise": 0.65, "seed": 7200 + i})
    slots.append({"id": sid, "key": f"{sid}s", "end_key": f"{sid}e", "length": 85, "step": 1, "frames": [F(a) - F(A), F(b) - F(A)],
                  "bars": [a, b], "composite": "none", "prompt": action, "seed": 171200000 + i})
plan = {"title": "Cosmic Rain - one-minute start+landing test (bars 30-66)", "fps": FPS, "style": STYLE,
        "audio": {"track": "D:/AvasthaAI/CosmicRain-20261025/inputs/track.wav", "start": PHASE + A * BAR}, "slots": slots}
(R / "stills.json").write_text(json.dumps(stills, indent=1), encoding="utf-8")
(R / "plan.json").write_text(json.dumps(plan, indent=1), encoding="utf-8")
print(len(slots), "shots,", slots[-1]["frames"][1] / FPS, "s")
