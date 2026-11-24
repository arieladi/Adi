// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace adi::mix::deesser {

enum class Mode { Allround, SingleVocal };
enum class KeySource { Internal, External };
enum class LinkMode { Mid, Side };
enum class Monitor { Normal, Sidechain, Removed };
enum class Band { Wide, Split };
enum class Quality { BaseRate, Times2, Times4 };

// All settings are fixed for a prepared stream. Physical units are explicit;
// only stereoLink uses the unit control convention (0.5 means 100% linked).
struct Settings {
    Mode mode = Mode::Allround;
    KeySource keySource = KeySource::Internal;
    LinkMode linkMode = LinkMode::Mid;
    Monitor monitor = Monitor::Normal;
    Band band = Band::Wide;
    Quality quality = Quality::BaseRate;
    float thresholdDb = -30;
    float rangeDb = 12;
    float highPassHz = 3000;
    float lowPassHz = 14000;
    float stereoLink = .5f;
    float inputGain = 1;
    float outputGain = 1;
    float inputPan = 0;
    float outputPan = 0;
    float lookaheadMs = 0;
    bool operator==(const Settings &) const = default;
};

} // namespace adi::mix::deesser
