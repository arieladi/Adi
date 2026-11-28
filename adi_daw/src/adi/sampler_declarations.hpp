// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <string_view>
namespace adi::device {
// DEV-21: the Sampler's parameters and its 32 sample slots, written in the
// [adi.param] and [adi.sample] record syntax so parsePdDeclarations reads them
// as it reads a canvas. It is NOT a Pd canvas: makeNativePdDevice never matches
// it, so no Pd patch can become the Sampler. Each id is the Sampler core's
// parameter index + 1; for ids 1..23 the line's range, default, unit and curve
// is the matching line of pd/devices/OneShot.pd (test_sampler checks they
// agree). Ids are fixed: a new parameter takes the next id, and its default is
// the sound the Sampler had without it, because a project saved before it
// existed opens with that default (SC-483: 25 to 51).
// Slot N is a zone over every key and velocity until its zone table says otherwise.
inline constexpr std::string_view samplerDeclarations = R"ADI_DECL(#N canvas 0 0 600 900 12;
#X obj 20 20 adi.param \$0 1 -36 36 -12 dB lin Volume;
#X obj 20 40 adi.param \$0 2 -48 48 0 st int Transpose;
#X obj 20 60 adi.param \$0 3 .1 20000 .1 ms lin Attack;
#X obj 20 80 adi.param \$0 4 1 60000 600 ms lin Decay;
#X obj 20 100 adi.param \$0 5 -70 0 0 dB lin Sustain;
#X obj 20 120 adi.param \$0 6 1 60000 50 ms lin Release;
#X obj 20 140 adi.param \$0 7 1 32 6 - int Voices;
#X obj 20 160 adi.param \$0 8 0 1 1 - int Retrigger;
#X obj 20 180 adi.param \$0 9 0 1 1 - int Filter;
#X obj 20 200 adi.param \$0 10 0 3 0 - menu Filter_type Low-pass High-pass Band-pass Notch;
#X obj 20 220 adi.param \$0 11 0 1 1 - int 24_dB_(off:_12_dB);
#X obj 20 240 adi.param \$0 12 30 22000 22000 Hz lin Frequency;
#X obj 20 260 adi.param \$0 13 0 125 0 % lin Resonance;
#X obj 20 280 adi.param \$0 14 0 1 1 - int LFO;
#X obj 20 300 adi.param \$0 15 0 5 0 - menu LFO_shape Sine Square Triangle Saw_down Saw_up Random;
#X obj 20 320 adi.param \$0 16 .01 30 1 Hz lin LFO_rate;
#X obj 20 340 adi.param \$0 17 0 1 0 - int LFO_sync;
#X obj 20 360 adi.param \$0 18 .0625 32 1 quarter\ notes lin LFO_period;
#X obj 20 380 adi.param \$0 19 0 24 0 st lin LFO_pitch;
#X obj 20 400 adi.param \$0 20 0 100 0 % lin LFO_volume;
#X obj 20 420 adi.param \$0 21 0 100 0 % lin LFO_pan;
#X obj 20 440 adi.param \$0 22 0 8 0 octaves lin LFO_filter;
#X obj 20 460 adi.param \$0 23 0 100 35 % lin Vel_>_Vol;
#X obj 20 480 adi.param \$0 24 0 127 0 - lin Sample_select;
#X obj 20 500 adi.param \$0 25 0 1 0 - int Reverse;
#X obj 20 520 adi.param \$0 26 0 1 1 - int Loop_fade:_constant_power;
#X obj 20 540 adi.param \$0 27 0 1 0 - int Round_robin;
#X obj 20 560 adi.param \$0 28 0 3 0 - menu Round_robin_mode Forward Backward Random Other;
#X obj 20 580 adi.param \$0 29 0 2 1 - menu Interpolation None Normal Good;
#X obj 20 600 adi.param \$0 30 0 1 0 - int Snap;
#X obj 20 620 adi.param \$0 31 -50 50 0 cents lin Detune;
#X obj 20 640 adi.param \$0 32 0 100 0 % lin Spread;
#X obj 20 660 adi.param \$0 33 -48 48 0 st int Zone_shift;
#X obj 20 680 adi.param \$0 34 0 1 0 - int Pitch_env;
#X obj 20 700 adi.param \$0 35 -48 48 0 st lin Pitch_env_amount;
#X obj 20 720 adi.param \$0 36 0 20000 0 ms lin Pitch_env_attack;
#X obj 20 740 adi.param \$0 37 1 60000 600 ms lin Pitch_env_decay;
#X obj 20 760 adi.param \$0 38 0 100 0 % lin Pitch_env_sustain;
#X obj 20 780 adi.param \$0 39 1 60000 50 ms lin Pitch_env_release;
#X obj 20 800 adi.param \$0 40 0 100 0 % lin Pitch_env_initial;
#X obj 20 820 adi.param \$0 41 0 100 100 % lin Pitch_env_peak;
#X obj 20 840 adi.param \$0 42 0 100 0 % lin Pitch_env_end;
#X obj 20 860 adi.param \$0 43 0 1 0 - int Filter_env;
#X obj 20 880 adi.param \$0 44 -72 72 0 st lin Filter_env_amount;
#X obj 20 900 adi.param \$0 45 0 20000 0 ms lin Filter_env_attack;
#X obj 20 920 adi.param \$0 46 1 60000 600 ms lin Filter_env_decay;
#X obj 20 940 adi.param \$0 47 0 100 0 % lin Filter_env_sustain;
#X obj 20 960 adi.param \$0 48 1 60000 50 ms lin Filter_env_release;
#X obj 20 980 adi.param \$0 49 0 100 0 % lin Filter_env_initial;
#X obj 20 1000 adi.param \$0 50 0 100 100 % lin Filter_env_peak;
#X obj 20 1020 adi.param \$0 51 0 100 0 % lin Filter_env_end;
#X obj 300 20 adi.sample \$0 1 Sample_1;
#X obj 300 40 adi.sample \$0 2 Sample_2;
#X obj 300 60 adi.sample \$0 3 Sample_3;
#X obj 300 80 adi.sample \$0 4 Sample_4;
#X obj 300 100 adi.sample \$0 5 Sample_5;
#X obj 300 120 adi.sample \$0 6 Sample_6;
#X obj 300 140 adi.sample \$0 7 Sample_7;
#X obj 300 160 adi.sample \$0 8 Sample_8;
#X obj 300 180 adi.sample \$0 9 Sample_9;
#X obj 300 200 adi.sample \$0 10 Sample_10;
#X obj 300 220 adi.sample \$0 11 Sample_11;
#X obj 300 240 adi.sample \$0 12 Sample_12;
#X obj 300 260 adi.sample \$0 13 Sample_13;
#X obj 300 280 adi.sample \$0 14 Sample_14;
#X obj 300 300 adi.sample \$0 15 Sample_15;
#X obj 300 320 adi.sample \$0 16 Sample_16;
#X obj 300 340 adi.sample \$0 17 Sample_17;
#X obj 300 360 adi.sample \$0 18 Sample_18;
#X obj 300 380 adi.sample \$0 19 Sample_19;
#X obj 300 400 adi.sample \$0 20 Sample_20;
#X obj 300 420 adi.sample \$0 21 Sample_21;
#X obj 300 440 adi.sample \$0 22 Sample_22;
#X obj 300 460 adi.sample \$0 23 Sample_23;
#X obj 300 480 adi.sample \$0 24 Sample_24;
#X obj 300 500 adi.sample \$0 25 Sample_25;
#X obj 300 520 adi.sample \$0 26 Sample_26;
#X obj 300 540 adi.sample \$0 27 Sample_27;
#X obj 300 560 adi.sample \$0 28 Sample_28;
#X obj 300 580 adi.sample \$0 29 Sample_29;
#X obj 300 600 adi.sample \$0 30 Sample_30;
#X obj 300 620 adi.sample \$0 31 Sample_31;
#X obj 300 640 adi.sample \$0 32 Sample_32;
)ADI_DECL";
inline constexpr std::string_view samplerUid = "org.adidaw.builtin.sampler";
// SC-483: the state stream that holds the 32 slots' zones (root key, tuning,
// level, pan, start and end, loop, and the key, velocity and sample-select
// ranges with their fades), beside "state" and "sample:1" to "sample:32".
inline constexpr std::string_view samplerZonesRole = "zones";
}  // namespace adi::device
