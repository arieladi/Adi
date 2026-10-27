"""Full-track storyboard for Cosmic Rain (7:43, 276 bars at 143 BPM) and the plan builder.

usage: storyboard_full.py <out plan.json>
Method approved by the user on the 16s test (2026-10-27): one readable subject per scene, keyframe
still first, each subject animated from its own still, symmetry by compositing, cuts on bar lines.
Section layout comes from analyse_track.py (track-bars.json): drops at bars 32, 64, 96, 208, 224.

Slot kinds:
  calm  - 2 bars (or the span given), one Wan clip at normal speed + slow push
  drop  - 2 bars per subject: bar A from the still, bar B continues from bar A's last frame,
          both at double speed (81 generated frames -> 40), like the approved test
  half  - half-bar riser cut (41 frames at double speed -> 20)
  test  - reused slot from the approved 16s test (bars 28-38)
"""
import hashlib, json, sys

BPM, PHASE, FPS = 143, 0.0215, 24
BAR = 240 / BPM
END_FRAME = round(463.285 * FPS)  # track end (11119)


def F(bar):
    return round(FPS * (PHASE + bar * BAR))


STYLE = ("psychedelic psytrance poster illustration, visionary art, bold clean ink outlines, cel shading, vivid "
         "saturated silver, cyan, deep ultramarine and gold palette with magenta accents, glowing highlights, high "
         "contrast, intricate ornamental detail, centered composition")
GODDESS = ("the Rain Keeper goddess, pale silver-blue skin, long flowing silver hair, a golden headband with three "
           "round turquoise gems, gold teardrop earrings, gold filigree necklace, silver raindrop tears on her cheeks")

# The user's own story moments (2026-10-27) use these recurring characters so they read as the same people.
ELDERS = ("a circle of tribal elders with long white hair, painted faces and feather and bead ornaments, "
          "playing large glowing frame drums and djembes")
FAMILY = "women and children with painted faces and glowing beads"
ALIENS = ("tall slender luminous alien beings with smooth silver-blue skin, large dark almond eyes and glowing cyan "
          "markings on their bodies")
TOKENS = {"{G}": GODDESS, "{E}": ELDERS, "{F}": FAMILY, "{A}": ALIENS}

# (kind, start bar, end bar, composite, still subject, motion, extras)
# Bars may be fractional for half-bar cuts. Tokens above are expanded in subjects and motions.
# kind: calm (normal speed), drop (2 bars, bar B chained), one (single slot at double speed), half (riser cut).
# extras: user=True marks a user story moment; dissolve_in=N crossfades from the previous slot (clock morphs);
# no_portal=True suppresses the automatic portal after a half-bar cut.
S = []
def add(kind, a, b, comp, subject, motion, **extras):
    for k, v in TOKENS.items():
        subject, motion = subject.replace(k, v), motion.replace(k, v)
    S.append(dict(kind=kind, a=a, b=b, comp=comp, subject=subject, motion=motion, **extras))

# --- Chapter 1: Arrival (intro, after the original 8 s opening) -----------------------------------
C = "Arrival"
add("calm", "open", 7, "mirror", "a vast black hole with a blazing golden accretion ring in deep space, silver rain falling into it from above like a waterfall of light", "The camera drifts slowly toward the golden ring while streams of silver rain pour into the black hole.")
add("calm", 7, 9, "none", "silver raindrops falling through deep space past the viewer, each raindrop reflecting a tiny spiral galaxy", "Raindrops fall steadily past the camera, the galaxies inside them glitter and turn.")
add("calm", 9, 11, "none", "{G} floating asleep in deep space, curled gently, her silver hair spreading out like a nebula, eyes closed", "She floats and turns slowly, her hair drifts like a nebula, raindrops orbit her.")
add("calm", 11, 13, "mirror", "towering rain clouds made of stars and crystal, soft lightning of light glowing inside them", "The star clouds swell and glow, soft lightning flickers inside, rain begins to fall.")
add("calm", 13, 15, "none", "one giant crystal raindrop falling toward a dark blue ocean planet, the planet glowing below", "The giant raindrop falls toward the ocean planet, the camera follows it down.")
add("calm", 15, 17, "mirror", "a crown-shaped splash rising from a dark ocean where a raindrop landed, concentric glowing rings spreading", "The crown splash rises and opens, rings of light spread outward across the water.")
add("calm", 17, 18, "radial8", "top-down view of glowing concentric ripples on a dark ocean forming an ornate mandala", "Ripples expand outward and the mandala slowly turns.")

# --- Chapter 2: Awakening (ambient build) --------------------------------------------------------
add("calm", 18, 20, "mirror", "underwater view looking up at rain hitting the ocean surface, golden light rays piercing the deep blue water", "Raindrops hit the surface above, light rays sway, bubbles rise toward the camera.")
add("calm", 20, 22, "none", "five glowing jellyfish rising slowly side by side in a row in dark deep water, long luminous tentacles", "The jellyfish pulse and rise together, tentacles trailing gracefully.")
add("calm", 22, 24, "mirror", "an ancient crystal temple on the ocean floor, glowing pillars, a beam of light from above", "The camera glides toward the temple, the light beam brightens, particles drift.")
add("calm", 24, 26, "none", "{G} face just beneath the water surface, eyes closed, tiny bubbles around her lips", "Water ripples over her face, bubbles rise, her hair waves gently.")
add("calm", 26, 28, "radial8", "a spiral of glowing raindrops forming around a small bright black hole star", "The spiral of raindrops turns faster and tightens around the star.")

# --- Approved 16s test: bar 28-30 (goddess) is reused (see build()); 30-38 is now the user's story ---

# --- User story 0:50-1:04: elders drum, family joins on the drop, clock, brain, fast clock ----------
add("calm", 30, 31, "mirror", "{E}, seen from the front around a fire, rain falling, cosmic sky", "The elders strike their drums together in a steady rhythm, firelight flickering, rain falling.", user=True)
add("calm", 31, 32, "none", "close-up of an elder's weathered hands raised above a glowing frame drum, dust and raindrops in the air", "The hands strike the glowing drum and sparks of light fly up.", user=True)
add("drop", 32, 33.5, "mirror", "{E} joined by {F}, everyone drumming together in a big circle, glowing drums, rain and cosmic light", "Everyone drums hard together, the women and children join in with joy, light bursts from every drum hit.", user=True)
add("one", 33.5, 34.5, "none", "a giant ornate golden clock face formed from the circle of drummers seen from above, the drums are the numerals, glowing clock hands", "The clock hands sweep around and everything ticks into place.", user=True, dissolve_in=8)
add("calm", 34.5, 35, "mirror", "{E} and {F} drumming together in a circle, calm and glowing", "They keep drumming, relaxed and steady.", user=True, dissolve_in=6)
add("one", 35, 36, "mirror", "a glowing human brain made of light with neurons firing, electric cyan and gold synapses, black space", "The camera flies fast through the firing neurons toward a bright light.", user=True)
add("drop", 36, 37.5, "mirror", "glowing neurons turning into streams of silver rain falling over a dark ocean", "The neuron lights stretch into rain and pour down toward the ocean.", user=True)
add("half", 37.5, 38, "none", "a fast spinning golden clock face with glowing hands on black", "The clock hands spin very fast.", user=True)

# --- Chapter 3: Rain Ocean (drop 1, bars 38-62) ---------------------------------------------------
drop1 = [
    ("none", "a giant whale made of starlight leaping out of a dark glowing ocean, water streaming off it", "The star whale leaps up out of the ocean, water and stars spray outward."),
    ("none", "seven identical cute glowing raindrop spirits with small faces dancing in a straight row on a dark mirror ocean", "The raindrop spirits bounce and step in sync, arms swinging, ripples under their feet."),
    ("mirror", "a huge ocean wave curling over into a glowing tunnel of silver light", "The camera surfs fast through the curling wave tunnel toward the light."),
    ("mirror", "a giant octopus made of glowing water with golden rings on its tentacles, perfectly symmetrical, facing the viewer", "The octopus spreads its tentacles outward in a powerful pulse."),
    ("radial8", "top-down view of a bioluminescent coral reef forming a glowing mandala", "The coral mandala pulses with light and slowly rotates."),
    ("mirror", "a silver sea dragon spiraling upward out of the ocean toward the black hole", "The sea dragon spirals upward fast, water trailing from its scales."),
    ("radial8", "thousands of raindrops frozen in mid-air forming a perfect glowing sphere", "The sphere of raindrops spins and expands outward."),
    ("mirror", "a formation of manta rays made of light gliding toward the viewer over a dark ocean", "The manta rays glide forward in formation, wings rippling."),
    ("mirror", "the ocean surface splitting open down the middle to reveal a glowing galaxy beneath", "The ocean parts wider and the galaxy below blazes brighter."),
    ("none", "{G} rising out of the ocean up to her shoulders, water streaming from her silver hair, eyes closed", "She rises from the water, water pours from her hair, light spreads around her."),
    ("none", "a giant golden nautilus shell with crystal chambers glowing, floating above the ocean", "The nautilus rotates and its chambers light up one after another."),
    ("mirror", "a waterfall falling upward from the ocean into the black hole with a golden ring", "The water rushes upward into the black hole, the golden ring flares."),
]
b = 38
for comp, subj, mot in drop1:
    add("drop", b, b + 2, comp, subj, mot); b += 2
# User story 1:45: the elders drum again in the break before the 1:47 drop.
add("calm", 62, 63, "mirror", "{E} drumming in the rain at night under a glowing cosmic sky", "The elders drum steadily, building up, rain falling around them.", user=True)
add("calm", 63, 64, "none", "close-up of glowing drums being struck, raindrops bouncing on the drum skins", "Hands strike the drums faster, raindrops jump off the drum skins.", user=True)

# --- Chapter 4: Crystal Cathedral (drop 1b, bars 64-90) -------------------------------------------
drop1b = [
    ("mirror", "an endless cathedral corridor of crystal arches with silver rain falling inside", "The camera flies fast down the crystal corridor through the rain."),
    ("radial8", "a giant clockwork of golden gears and raindrops", "The golden gears turn and raindrops roll along them."),
    ("mirror", "a stained glass window of a rain goddess glowing in a dark cathedral", "The stained glass glows brighter and shatters outward into light."),
    ("none", "five identical crystal monk statues in a row with glowing cyan eyes, rain falling", "The monks' eyes ignite one after another, light beams sweep."),
    ("radial8", "prisms splitting beams of light into rainbows of cyan, gold and magenta", "The prisms rotate and the light beams sweep around."),
    ("mirror", "a staircase of water spiraling upward into the stars, impossible geometry", "The camera climbs fast up the spiral staircase of water."),
    ("mirror", "giant golden temple doors opening onto a blazing starfield", "The temple doors swing open and starlight floods out."),
    ("mirror", "a crystal tree whose leaves are glowing raindrops, black background", "The crystal tree grows and its raindrop leaves light up."),
    ("radial8", "sacred geometry, a glowing golden Metatron's cube with floating crystals", "The sacred geometry rotates and the crystals orbit."),
    ("mirror", "a hall of mirrors with silver rain falling, infinite reflections", "The camera moves forward through the infinite reflections."),
    ("mirror", "a giant glass bell glowing in a crystal tower", "The bell swings and rings, a shockwave of light rings spreads."),
    ("mirror", "an owl made of crystal with golden eyes, wings spread wide, facing the viewer", "The crystal owl beats its wings and its eyes flare."),
    ("mirror", "a diamond eye inside a golden pyramid radiating light beams", "The eye opens and the light beams pulse outward."),
]
# User story 1:47 (drop at bar 64): aliens arrive and play impossible synths with the elders (bars 64-76).
aliens = [
    ("mirror", "a glowing alien ship of light descending from a cosmic sky above {E}", "The ship of light descends slowly over the drummers, beams of light sweep the circle."),
    ("mirror", "{A} standing behind impossible floating crystal synthesizers with hundreds of glowing keys and holographic waveforms", "The aliens play the glowing synthesizers, the waveforms pulse in fast steady rhythm."),
    ("mirror", "{E} drumming in front while {A} play floating crystal synthesizers behind them, cosmic sky", "The elders drum and the aliens play together, light pulses on every beat."),
    ("radial8", "close-up of an alien synthesizer made of light, glowing keys and spinning holographic waveform rings", "The holographic waveforms spin and pulse in fast sixteenth-note rhythm."),
    ("none", "{F} dancing joyfully around the drum circle while {A} play glowing synths and {E} drum", "The children and women dance, the elders drum, the aliens play, light everywhere."),
    ("mirror", "rivers of glowing sound waves flowing from alien synthesizers up into the sky", "The sound waves ripple upward in fast rhythmic pulses."),
]
b = 64
for comp, subj, mot in aliens:
    add("drop", b, b + 2, comp, subj, mot, user=True); b += 2
# Crystal Cathedral resumes for the rest of the drop (bars 76-90).
for k in (0, 2, 4, 5, 6, 11, 12):
    comp, subj, mot = drop1b[k]
    add("drop", b, b + 2, comp, subj, mot); b += 2
assert b == 90

# --- Break and riser into drop 2 (bars 90-96); user story 2:34: the clock finds us ----------------
add("calm", 90, 92, "none", "{G} meditating on a floating rock under soft rain, eyes closed, hands resting in her lap", "Rain falls softly around her, her hair drifts, the rock turns slowly.")
add("calm", 92, 94, "mirror", "a giant ornate golden clock slowly appearing out of darkness and rain, glowing hands", "The clock slowly emerges and its hands begin to move, pulling the camera toward it.", user=True)
add("calm", 94, 95, "mirror", "flying toward the center of a giant golden clock, the hands spinning around the camera", "The clock hands spin faster as the camera is pulled into the center.", user=True)
add("half", 95, 95.5, "none", "looking down through the glass center of a golden clock at {E} far below", "The camera falls through the clock toward the drummers.", user=True)
add("half", 95.5, 96, "mirror", "diving down fast toward {E} from above", "The camera dives fast toward the drum circle.", user=True)

# --- User story 2:41-2:45 (drop 2 at bar 96): new synths and speakers, everyone dances; fast clock;
#     heaven: a beautiful woman and man become golden statues, then elements. ----------------------
add("drop", 96, 97.5, "mirror", "{A} with towering new synthesizers and giant glowing speaker towers, {E} drumming and {F} dancing in front", "Everyone dances, the speaker towers pulse with light on the beat, the aliens play.", user=True)
add("half", 97.5, 98, "none", "a fast spinning golden clock face with glowing hands on black", "The clock hands spin very fast.", user=True)
add("drop", 98, 100, "mirror", "a heavenly paradise of golden light and clouds, a beautiful woman and a beautiful man facing each other, turning into gleaming golden statues", "Gold spreads over their bodies and they become shining golden statues.", user=True)
add("drop", 100, 102, "mirror", "two golden statues of a woman and a man dissolving into the elements, fire, water, air and earth swirling from them", "The statues break apart into swirling fire, water, wind and earth.", user=True)

# --- Chapter 5a: Storm Spirits (drop 2, bars 96-128) ----------------------------------------------
storm = [
    ("mirror", "a thunderbird made of lightning and rain bursting out of a storm vortex, wings spread, facing the viewer", "The thunderbird bursts forward out of the vortex, lightning crackling."),
    ("mirror", "two rain dragons coiling around a column of golden lightning", "The dragons spiral up the lightning column."),
    ("mirror", "a storm cloud shaped like the face of a sky god with a beard of lightning", "The storm face's eyes flash and lightning pours from its beard."),
    ("mirror", "a tree of golden lightning branching across a dark sky", "The lightning tree branches outward fast across the sky."),
    ("radial8", "inside a tornado of rain and stars looking up", "The tornado spins fast around the camera."),
    ("none", "five drummers made of thunderclouds in a row drumming on glowing drums", "The cloud drummers strike their drums together, light bursts on each hit."),
    ("radial8", "lightning striking a crystal ocean creating a glowing fractal pattern", "Lightning strikes and the fractal spreads outward."),
    ("mirror", "an eagle made of silver rain diving straight toward the viewer, wings spread", "The rain eagle dives fast toward the camera."),
    ("none", "a storm wolf made of clouds and aurora howling on a crystal cliff", "The wolf howls and the aurora ripples across the sky."),
    ("radial8", "top-down view of rain spirit dancers spinning in a circle", "The dancers spin around the circle."),
    ("none", "macro of diamond hailstones bouncing on black glass with sparks", "Hailstones bounce and scatter sparks."),
    ("radial8", "top-down view of a cyclone eye with a golden ring", "The cyclone spins and the golden ring pulses."),
    ("mirror", "a lightning serpent biting its own tail in a perfect circle, ouroboros", "The lightning ouroboros rotates and crackles."),
    ("mirror", "a rainbow bridge of light forming through a dark storm", "The rainbow bridge extends forward through the storm."),
    ("mirror", "a giant thunder drum of the gods glowing in the clouds", "The drum is struck and a shockwave of light rings spreads."),
    ("none", "{G} standing in the storm with glowing eyes, hands resting over her heart, lightning behind her", "Lightning flashes behind her and her hair whips in the wind."),
]
b = 102
for k in (0, 1, 2, 3, 4, 6, 7, 11):
    comp, subj, mot = storm[k]
    add("drop", b, b + 2, comp, subj, mot); b += 2
assert b == 118
# User story 3:18 (bar 118): the brain again, free your mind; a rooftop leap, flying free, becoming
# falling glowing letters (a nod to the reference the user named, not a copy of its characters).
add("drop", 118, 120, "mirror", "a glowing human brain made of light breaking free of chains of light, neurons firing, black space", "The chains of light shatter and the brain glows brighter, neurons flashing.", user=True)
add("drop", 120, 122, "none", "a man in a long black coat leaping across the gap between two skyscraper rooftops at night, city lights far below, rain", "He leaps in a huge arc between the rooftops, his coat flying behind him.", user=True)
add("drop", 122, 124, "none", "a man in a long black coat flying freely up through the clouds above a night city, arms at his sides", "He flies upward fast through the clouds, free.", user=True)
add("drop", 124, 126, "mirror", "a man's silhouette dissolving into cascading streams of glowing green and cyan code symbols falling like rain", "His body dissolves into streams of glowing symbols raining down.", user=True)
add("drop", 126, 128, "none", storm[15][1], storm[15][2])

# --- Chapter 5b: Cosmic Garden (bars 128-160) -----------------------------------------------------
garden = [
    ("mirror", "a forest of giant glowing mushrooms of light in the rain", "The mushrooms pulse with light as the camera flies between them."),
    ("radial8", "a giant fractal flower opening, mandala petals", "The fractal petals unfold outward."),
    ("none", "five crystal frogs sitting in a row on lotus pads in a rainy pond", "The frogs hop in sync and ripples spread."),
    ("mirror", "golden vines growing into a glowing spiral", "The golden vines grow and curl into the spiral."),
    ("mirror", "a butterfly with stained glass wings, perfectly symmetrical, facing the viewer", "The butterfly beats its wings and light shines through them."),
    ("none", "a meadow of glowing dandelions releasing seeds made of stars", "The dandelions release their star seeds into the air."),
    ("mirror", "a tree of life with glowing rivers as roots", "Light flows up through the roots and the tree glows."),
    ("none", "a hummingbird made of light drinking from a glowing raindrop", "The hummingbird hovers, wings blurring, drinking the raindrop."),
    ("radial8", "a sunflower with a fibonacci spiral of golden seeds", "The sunflower spiral rotates and glows."),
    ("mirror", "a fern frond unrolling into a fractal spiral of light", "The fern unrolls into its fractal spiral."),
    ("mirror", "a tunnel of glowing hexagon honeycomb with bees of light", "The camera flies through the honeycomb tunnel."),
    ("mirror", "a peacock made of rain spreading its glowing tail, facing the viewer", "The peacock spreads its tail and the eyes on it glow."),
    ("radial8", "a flower bud opening to reveal a tiny galaxy inside", "The bud opens and the galaxy spins."),
    ("mirror", "a forest spirit deer with antlers of crystal, facing the viewer", "The deer raises its head and its antlers glow."),
    ("none", "glowing seeds falling onto dark ground and bursting into flowers", "The seeds land and burst into blooming flowers."),
    ("radial8", "top-down view of a giant water lily mandala", "The water lily mandala opens and turns."),
]
b = 128
for comp, subj, mot in garden:
    add("drop", b, b + 2, comp, subj, mot); b += 2

# --- Chapter 5c: Galaxy Whale (bars 160-189) ------------------------------------------------------
whale = [
    ("none", "a galaxy whale swimming through pink and blue nebula clouds", "The galaxy whale swims forward through the nebula."),
    ("none", "planets lined up like pearls with glowing rings", "The planets drift past the camera."),
    ("mirror", "a comet made of rain streaking toward the viewer", "The comet streaks toward the camera, rain trailing."),
    ("none", "a fleet of cosmic jellyfish floating through space", "The jellyfish pulse and drift forward."),
    ("none", "macro of a whale eye with galaxies reflected inside", "The whale eye blinks and the galaxies swirl."),
    ("mirror", "a crystal astronaut floating among raindrops, facing the viewer", "The astronaut drifts and raindrops orbit around."),
    ("mirror", "flying through the rings of a planet made of raindrops", "The camera flies fast through the rings."),
    ("mirror", "a nebula shaped like a goddess face", "The nebula face glows and its eyes open."),
    ("radial8", "a warp tunnel of stars", "The stars stream past in a warp."),
    ("none", "constellation animals connected by glowing lines", "The constellation lines draw themselves and glow."),
    ("mirror", "space rain falling on a silver moon surface making ripples in the dust", "Rain hits the moon and ripples spread."),
    ("radial8", "top-down view of a spiral galaxy", "The galaxy spiral rotates."),
    ("mirror", "the galaxy whale diving into a black hole with a golden ring", "The whale dives into the black hole and the ring flares."),
]
b = 160
for comp, subj, mot in whale:
    add("drop", b, b + 2, comp, subj, mot); b += 2
add("calm", 186, 188, "mirror", "a black hole swallowing streams of light, the golden ring dimming", "Light streams into the black hole and the ring slowly dims.")
add("calm", 188, 189, "none", "a single glowing raindrop falling in darkness", "The raindrop falls slowly through darkness.")

# --- Chapter 6: Dream (breakdown, bars 189-208) ---------------------------------------------------
add("calm", 189, 191, "none", "{G} sleeping inside a floating bubble of water, curled peacefully", "The bubble drifts and wobbles, her hair floats inside.")
add("calm", 191, 193, "none", "floating memory bubbles, each containing a tiny glowing scene of a whale, a crystal cathedral and a storm", "The memory bubbles float past the camera, scenes glowing inside.")
add("calm", 193, 195, "mirror", "golden light rays through dark deep water", "The light rays sway slowly and particles drift.")
add("calm", 195, 197, "none", "a silver feather falling slowly through soft rain", "The feather drifts down slowly, turning.")
add("calm", 197, 199, "mirror", "a calm moonlit ocean reflecting a black hole with a golden ring", "The ocean gently swells, the reflection shimmers.")
add("calm", 199, 201, "mirror", "a glowing raindrop resting inside an open lotus flower", "The lotus slowly opens wider and the raindrop glows.")
add("calm", 201, 203, "none", "a deer made of light drinking from a still lake reflecting stars", "The deer drinks and ripples spread across the stars.")
add("calm", 203, 205, "mirror", "snow of light falling on a sleeping city of crystal towers", "Light snow falls softly over the crystal city.")
add("calm", 205, 207, "none", "close-up of {G} face, her eyes slowly opening, glowing cyan", "Her eyes slowly open and glow, rain falls around her.")
add("half", 207, 207.5, "radial8", "a kaleidoscope ring of glowing cyan eyes", "The ring of eyes spins.")
add("half", 207.5, 208, "none", "extreme close-up of a glowing cyan pupil, falling into it", "The camera zooms fast into the pupil.")

# --- Chapter 7: Burst (drop 3, bars 208-215) ------------------------------------------------------
add("drop", 208, 210, "radial8", "an explosion of silver rain forming a giant mandala of light", "The mandala explodes outward in rings of light.")
add("drop", 210, 212, "mirror", "two mirrored goddess profiles facing each other with a burst of light between them", "Light bursts between the two faces, particles swirl.")
add("drop", 212, 214, "radial8", "a kaleidoscope of glowing raindrop spirits multiplying", "The raindrop spirits multiply and spin.")
add("calm", 214, 215, "radial8", "a burst of golden particles on black", "Golden particles burst outward.")

# --- Break and riser into the final drop (bars 215-224) -------------------------------------------
add("calm", 215, 217, "mirror", "drifting through glowing clouds at night in soft rain", "The camera drifts slowly through the clouds.")
add("calm", 217, 219, "mirror", "a lighthouse of crystal beaming light through a gentle storm", "The lighthouse beam sweeps slowly through the rain.")
add("calm", 219, 221, "none", "ocean waves rolling slowly under moonlight with silver foam", "The waves roll slowly toward the camera.")
add("calm", 221, 223, "mirror", "a black hole with a golden ring rising over the ocean horizon like a sunrise", "The black hole rises slowly over the horizon, light spreading.")
add("half", 223, 223.5, "radial8", "a ring of black holes with golden rings on black", "The ring of black holes spins fast.")
add("half", 223.5, 224, "none", "rushing straight into a black hole with a blazing golden ring", "The camera rushes into the black hole.")

# --- Chapter 8: Ascension (final drop, bars 224-268) ----------------------------------------------
ascend = [
    ("none", "{G} as a giant titan in space, a galaxy glowing over her heart, eyes glowing", "The galaxy over her heart blazes and her hair streams outward."),
    ("mirror", "silver rain turning into stars and rising upward", "The rain rises upward, turning into stars."),
    ("mirror", "a golden phoenix made of rain and fire, wings spread, facing the viewer", "The phoenix spreads its wings and rises."),
    ("mirror", "a cathedral made of galaxies", "The camera flies forward through the galaxy cathedral."),
    ("radial8", "a mandala made of repeated glowing goddess faces", "The mandala of faces rotates."),
    ("none", "glowing raindrop spirits riding on the back of a galaxy whale", "The whale swims forward and the spirits cheer."),
    ("mirror", "a silver serpent coiled around a blooming lotus of light", "The serpent rears and the lotus blooms."),
    ("mirror", "a giant cosmic eye opening in the sky", "The cosmic eye opens wide and light pours out."),
    ("mirror", "a ladder of golden light rising into a black hole", "The camera climbs the ladder of light fast."),
    ("none", "five crystal angels standing in a row with glowing wings", "The angels spread their wings together."),
    ("radial8", "a fractal spiral of gold and silver", "The fractal spiral rotates and zooms."),
    ("mirror", "a black hole blooming into a lotus of light", "The black hole opens into a blooming lotus."),
    ("mirror", "flying fast down a tunnel of falling silver rain and golden rings", "The camera flies very fast through the rain tunnel."),
    ("radial8", "top-down view of glowing figures dancing in a circle around a fire made of rain", "The figures dance around the circle."),
    ("mirror", "an ocean made of stars with glowing waves", "The star waves roll toward the camera."),
    ("mirror", "a cosmic tree of life with glowing planets as fruit", "The planets on the tree glow and orbit."),
    ("none", "{G} crowned with orbiting planets, eyes glowing, calm expression", "The planets orbit around her head."),
    ("radial8", "a kaleidoscope of a serpent, a whale and jellyfish", "The kaleidoscope rotates and pulses."),
    ("mirror", "a sun and a black hole orbiting each other like yin and yang", "The sun and the black hole orbit each other."),
    ("mirror", "a giant wave of light crashing toward the viewer", "The wave of light crashes toward the camera."),
    ("mirror", "a golden crown radiating beams of light, floating in space", "The crown's beams sweep and pulse."),
    ("radial8", "a final grand mandala of silver rain and gold", "The mandala expands and rotates."),
]
# User story 6:15 (final drop at bar 224): the drummers at full power in the storm; the aliens shield
# them from the lightning and play the lightning like synths. Then Ascension continues (bars 232-268).
storm_drums = [
    ("mirror", "{E} drumming at full power in heavy rain, cosmic rays streaking across the sky, wind and snow swirling", "The elders drum with full force, rain and snow whip around them, cosmic rays flash."),
    ("mirror", "lightning striking all around {E} who keep drumming in the storm, snow and wind", "Lightning strikes around the circle while the elders keep drumming hard."),
    ("mirror", "{A} raising a glowing dome shield of light over {E}, lightning bolts hitting the shield", "Lightning crashes into the shield and the aliens hold it, the drummers keep playing."),
    ("mirror", "{A} holding captured lightning bolts like glowing strings and playing them like a synthesizer, sparks of sound", "The aliens pluck and play the lightning bolts, waves of light and sound burst out."),
]
b = 224
for comp, subj, mot in storm_drums:
    add("drop", b, b + 2, comp, subj, mot, user=True); b += 2
for k, (comp, subj, mot) in enumerate(ascend):
    if k in (3, 9, 13, 17):
        continue
    add("drop", b, b + 2, comp, subj, mot); b += 2
assert b == 268

# --- Ending (bars 268-end) ------------------------------------------------------------------------
add("calm", 268, 270, "none", "macro of {G} eye with a whole galaxy reflected in it", "The camera pulls slowly back from the eye.")
add("calm", 270, 272, "none", "a spiral galaxy shrinking into a single glowing raindrop", "The galaxy slowly shrinks into a raindrop.")
add("calm", 272, 274, "mirror", "a glowing raindrop falling into a black hole with a calm golden ring", "The raindrop falls gently into the black hole.")
add("calm", 274, "end", "mirror", "a black hole fading into darkness while silver rain keeps falling", "The black hole slowly fades and the rain keeps falling.")


def length_for(n, step):
    # Smallest Wan length (4k+1) giving n frames at this step.
    need = (n - 1) * step + 1
    return need + (-(need - 1)) % 4


def build():
    test = json.load(open(sys.argv[2], encoding="utf-8")) if len(sys.argv) > 2 else None
    slots, scenes = [], []
    for i, s in enumerate(S):
        sid = f"K{i + 1:03d}"
        f0 = 192 if s["a"] == "open" else F(s["a"])
        f1 = END_FRAME if s["b"] == "end" else F(s["b"])
        key = "k" + hashlib.sha1(s["subject"].encode()).hexdigest()[:10]  # still name survives renumbering
        scenes.append({"id": sid, "key": key, "kind": s["kind"], "composite": s["comp"], "subject": s["subject"],
                       "user_story": bool(s.get("user"))})
        prompt = s["motion"]
        first = len(slots)
        if s["kind"] == "drop":
            mid = F(s["a"] + 1)
            for part, (g0, g1) in (("a", (f0, mid)), ("b", (mid, f1))):
                n = g1 - g0
                slots.append({"id": f"{sid}{part}", "scene": sid, "key": key, "frames": [g0, g1], "length": length_for(n, 2), "step": 2,
                              "composite": s["comp"], "prompt": prompt if part == "a" else prompt + " The motion continues and intensifies.",
                              **({"chain_from": f"{sid}a"} if part == "b" else {})})
        else:
            step = 2 if s["kind"] in ("half", "one") else 1
            n = f1 - f0
            slot = {"id": sid, "scene": sid, "key": key, "frames": [f0, f1], "length": length_for(n, step), "step": step,
                    "composite": s["comp"], "prompt": prompt}
            if s["kind"] == "calm" and n > 60:
                slot["push"] = [1.0, 1.1]
            slots.append(slot)
        for k in ("dissolve_in", "no_portal"):
            if k in s:
                slots[first][k] = s[k]
    # Insert the approved test's goddess slot (bars 28-30); the rest of the test window is the user's story.
    if test:
        tslots = [dict(t, id="T" + t["id"], take_root="D:/AvasthaAI/CosmicKeyframes16-20261027",
                       take=t.get("take", t["id"]), reused=True) for t in test["slots"] if t["frames"][1] <= F(30)]
        at = next(i for i, s in enumerate(slots) if s["frames"][0] >= F(30))
        slots[at:at] = tslots
    # Portals: the first slot after a half-bar riser opens through a growing circle, as in the test.
    kind = {sc["id"]: sc["kind"] for sc in scenes}
    for prev, cur in zip(slots, slots[1:]):
        if kind.get(prev.get("scene")) == "half" and kind.get(cur.get("scene")) != "half"                 and not cur.get("no_portal") and not cur.get("dissolve_in"):
            cur["portal_in"] = 6
    # Contiguity check.
    pos = 192
    for s in slots:
        assert s["frames"][0] == pos, (s["id"], s["frames"], pos)
        pos = s["frames"][1]
    assert pos == END_FRAME, pos
    gens = [s for s in slots if not s.get("reused")]
    plan = {"title": "Cosmic Rain - full track, keyframe method", "fps": FPS, "bpm": BPM, "phase": PHASE,
            "opening": {"source": "D:/AvasthaAI/CosmicRain-20261025/inputs/opening.mp4", "frames": [0, 192]},
            "audio": {"track": "D:/AvasthaAI/CosmicRain-20261025/inputs/track.wav", "start_frame": 0, "end_frame": END_FRAME},
            "style": STYLE, "scenes": scenes, "slots": slots,
            "stats": {"scenes": len(scenes), "user_story_scenes": sum(sc["user_story"] for sc in scenes),
                      "slots": len(slots), "wan_jobs": len(gens),
                      "wan_frames": sum(s["length"] for s in gens)}}
    json.dump(plan, open(sys.argv[1], "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    print(json.dumps(plan["stats"]))


if __name__ == "__main__":
    build()
