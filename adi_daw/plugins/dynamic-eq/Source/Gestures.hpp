// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>

namespace adi::eq {
// Order matches pinned ZL PFilterType; limits match PFreq/PGain/PTargetGain/PQ.
enum class Shape {
    Bell,
    LowShelf,
    LowPass,
    HighShelf,
    HighPass,
    Notch,
    BandPass,
    TiltShelf,
    FlatTilt,
    AllPass,
    FlatGain
};
struct Band {
    double frequency = 1000, gain = 0, q = .707, range = 0;
    Shape shape = Shape::Bell;
    int slope = 1; // ZL POrder: 12 dB/oct.
    bool bypass = false, dynamic = false;
};
struct Modifiers {
    bool control = false, alt = false, shift = false;
};
enum class Kind { Drag, Wheel, Click, DoubleClick, RightClick, Create };
enum class Axis { None, Frequency, Vertical };
enum class Action { None, EditValues, Menu, CreateBand };
// Drag deltas are measured from gesture start, in octaves, dB and Q octaves.
// Wheel uses the same units per detent. The GUI supplies its pixel mapping.
struct Input {
    Kind kind = Kind::Drag;
    double dx = 0, dy = 0, wheel = 0;
    Modifiers mods;
    Axis axis = Axis::None;
};
struct Result {
    Band band;
    Axis axis;
    Action action = Action::None;
};
inline bool hasGain(Shape s) {
    return s != Shape::LowPass && s != Shape::HighPass && s != Shape::Notch && s != Shape::BandPass &&
           s != Shape::AllPass;
}
inline Band clamp(Band b) {
    b.frequency = std::clamp(b.frequency, 10.0, 160000.0);
    b.gain = std::clamp(b.gain, -30.0, 30.0);
    b.q = std::clamp(b.q, .025, 25.0);
    // ZL stores the end gain, not a signed range. Keep that end inside its bounds.
    b.range = std::clamp(b.range, -30.0 - b.gain, 30.0 - b.gain);
    return b;
}
inline Result gesture(Band b, const Input &in) {
    Result out{b, in.axis};
    if (!std::isfinite(in.dx) || !std::isfinite(in.dy) || !std::isfinite(in.wheel))
        return out;
    // ADI sensitivity choice: Shift is one tenth of the ordinary step.
    const double fine = in.mods.shift ? .1 : 1.0;
    if (in.kind == Kind::DoubleClick)
        out.action = Action::EditValues;
    else if (in.kind == Kind::RightClick)
        out.action = Action::Menu;
    else if (in.kind == Kind::Click) {
        if (in.mods.alt && in.mods.control)
            b.shape = static_cast<Shape>((static_cast<int>(b.shape) + 1) % 11);
        else if (in.mods.alt && in.mods.shift)
            b.slope = (b.slope + 1) % 7;
        else if (in.mods.alt)
            b.bypass = !b.bypass;
    } else if (in.kind == Kind::Wheel) {
        const double step = in.wheel * fine;
        if (in.mods.alt && in.mods.control) {
            const double gain = std::clamp(b.gain + step, -30.0, 30.0);
            b.range -= gain - b.gain;
            b.gain = gain; // Preserve target gain.
            b.dynamic = true;
        } else if (in.mods.alt) {
            b.range += step;
            b.dynamic = true;
        } else if (in.mods.control)
            b.gain += step;
        else
            b.q *= std::exp2(step);
    } else {
        if (in.kind == Kind::Create) {
            out.action = Action::CreateBand;
            b.dynamic = in.mods.alt;
        }
        if (in.mods.alt && in.kind == Kind::Drag && out.axis == Axis::None && (in.dx != 0 || in.dy != 0))
            out.axis = std::abs(in.dx) > std::abs(in.dy) ? Axis::Frequency : Axis::Vertical;
        const bool locked = in.mods.alt && in.kind == Kind::Drag;
        if (!locked || out.axis != Axis::Vertical)
            b.frequency *= std::exp2(in.dx * fine);
        if (!locked || out.axis != Axis::Frequency) {
            if (in.mods.control || !hasGain(b.shape))
                b.q *= std::exp2(in.dy * fine);
            else
                b.gain += in.dy * fine;
        }
        // ADI creation policy: a new dynamic band moves from the drawn gain
        // toward unity at full action. Wheel then edits that range independently.
        if (in.kind == Kind::Create && in.mods.alt)
            b.range = -b.gain;
    }
    out.band = clamp(b);
    return out;
}
} // namespace adi::eq
