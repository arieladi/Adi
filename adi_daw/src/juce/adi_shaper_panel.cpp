// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi_shaper_panel.hpp"
#include "adi/dsp/adi_shaper.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace adi::ui {
namespace {
using Panel = AdiShaperPanel;
using Rect = juce::Rectangle<float>;
using Pos = juce::Point<float>;
using Core = dsp::AdiShaper;
constexpr std::array<const char *, 8> names{"Volume", "Width", "Pan",   "Filter",
                                            "Sweep",  "Drive", "Crush", "Time"};
constexpr std::array<juce::uint32, 8> colours{0xffffd638, 0xffdf6c9e, 0xffff8a48, 0xff61baf8,
                                              0xff49c8b6, 0xfffa6960, 0xffb985f6, 0xffa3ce49};
const juce::Colour ink(0xffc5c5ca), dim(0xff95959e), plate(0xff17171c), rail(0xff08080b);
juce::Colour colour(int stage) { return juce::Colour(colours[static_cast<std::size_t>(stage)]); }
void label(juce::Graphics &g, juce::String s, Rect r, float size, juce::Colour c,
           juce::Justification j = juce::Justification::centred) {
    g.setColour(c);
    g.setFont(juce::FontOptions(size));
    g.drawText(s, r, j, true);
}
void box(juce::Graphics &g, Rect r, juce::Colour c, float radius = 3.f) {
    g.setColour(c);
    g.fillRoundedRectangle(r, radius);
}
juce::String percent(double v) { return juce::String(juce::roundToInt(100 * v)) + "%"; }
juce::String frequency(double hz) {
    return hz >= 1000 ? juce::String(hz / 1000, 2) + " kHz" : juce::String(hz, hz >= 10 ? 0 : 2) + " Hz";
}
juce::String cycle(double n) {
    const double bars = std::exp2(n * 12.0 - 7.0);
    if (bars < .999)
        return "1/" + juce::String(1.0 / bars, (std::abs(std::round(1.0 / bars) - 1.0 / bars) < .005) ? 0 : 2) +
               " bar";
    return juce::String(bars, std::abs(std::round(bars) - bars) < .005 ? 0 : 2) +
           (bars <= 1.001 ? " bar" : " bars");
}

constexpr std::array<const char *, 3> bandNames{"Low", "Mid", "High"};
constexpr std::array<const char *, 8> timeRanges{"Follow", "1 bar",  "2 bars", "4 bars",
                                                 "8 bars", "2.5 ms", "10 ms",  "20 ms"};
constexpr float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
bool inside(Rect r, Pos p) { return r.contains(p); }
Rect button(int i) { return {10.f, 238.f + 33.f * i, 31.f, 27.f}; }
Rect palette(int i) { return {10.f + i * 76.f, 549.f, 70.f, 22.f}; }
} // namespace

AdiShaperPanel::AdiShaperPanel() {
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setTitle("ADI-Shaper");
    setSize(1040, 636);
}
AdiShaperPanel::~AdiShaperPanel() { cancel(); }
void AdiShaperPanel::bind(Binding next) {
    cancel();
    binding_ = std::move(next);
    tabs_.fill(false);
    key_ = {};
    spectrum_.clear();
    for (int s = 0; s < (binding_.parameterCount >= 463 ? 8 : 7); ++s) {
        key_ = {s, 1, 0};
        auto m = mapping();
        if (s >= 3) {
            tabs_[static_cast<std::size_t>(s)] = read(m.on) >= .5;
            continue;
        }
        const int layout = juce::roundToInt(read(m.layout) * 3);
        bool active = layout != 0;
        for (int b = 0; b < 3; ++b) {
            if ((b == 0 && !(layout & 1)) || (b == 2 && !(layout & 2)))
                continue;
            key_.band = b;
            m = mapping();
            if (read(m.mix) == 0)
                continue;
            for (int i = 0; i < 8; ++i)
                active |= std::abs(read(m.points + i) - (s == 0 ? 1. : .5)) > 1.e-6;
            active |= binding_.wave && !binding_.wave(key_).empty();
        }
        tabs_[static_cast<std::size_t>(s)] = active;
    }
    if (std::none_of(tabs_.begin(), tabs_.end(), [](bool v) { return v; }))
        tabs_[0] = true;
    key_ = {static_cast<int>(std::find(tabs_.begin(), tabs_.end(), true) - tabs_.begin()), 1, 0};
    selected_ = -1;
    phase_ = -1;
    peaks_ = {};
    for (auto &t : traces_)
        t.clear();
    refresh();
}
void AdiShaperPanel::setPresetName(juce::String name) {
    auto recalled = binding_;
    recalled.presetName = std::move(name);
    bind(std::move(recalled));
}
void AdiShaperPanel::refresh() {
    pointsDirty_ = true;
    repaint();
}
void AdiShaperPanel::setTrace(Trace input, Trace output, float left, float right, float phase) {
    const std::array<std::span<const float>, 4> source{input.minimum, input.maximum, output.minimum,
                                                       output.maximum};
    for (std::size_t i = 0; i < source.size(); ++i) {
        auto v = source[i].first(std::min(source[i].size(), std::size_t{2048}));
        traces_[i].assign(v.begin(), v.end());
    }
    peaks_ = {left, right};
    phase_ = phase;
    repaint();
}
void AdiShaperPanel::select(Key k) {
    cancel();
    k.stage = std::clamp(k.stage, 0, binding_.parameterCount >= 463 ? 7 : 6);
    k.band = std::clamp(k.band, 0, 2);
    if (k.stage != 3 && k.stage != 4 && k.stage != 6)
        k.lane = 0;
    k.lane = std::clamp(k.lane, 0, 1);
    key_ = k;
    pointsDirty_ = true;
    tabs_[static_cast<std::size_t>(k.stage)] = true;
    selected_ = -1;
    repaint();
}
AdiShaperPanel::Map AdiShaperPanel::mapping() const {
    Map m;
    const int s = key_.stage, b = key_.band;
    if (s < 3) {
        m.layout = 2 + Core::stageParameterCount * s;
        m.low = m.layout + 1;
        m.high = m.low + 1;
        const int base = m.layout + 3 + Core::bandParameterCount * b;
        m.mix = base;
        m.clock = base + 1;
        m.cycle = base + 2;
        m.rate = base + 3;
        m.interpolation = base + 4;
        m.points = base + 5;
    } else {
        static constexpr std::array<int, 5> starts{Core::FilterOn + 1, Core::SweepOn + 1, Core::DriveOn + 1,
                                                   Core::CrushOn + 1, 416};
        m.on = starts[static_cast<std::size_t>(s - 3)];
        m.layout = m.on + 1;
        m.low = m.on + 2;
        m.high = m.on + 3;
        const int stride = s == 7 ? 14 : (s == 5 ? Core::driveBandParameterCount : Core::modBandParameterCount);
        const int base = m.on + 6 + b * stride;
        m.type = base;
        m.mix = base + 1;
        m.clock = base + 2;
        m.cycle = base + 3;
        m.rate = base + 4;
        m.interpolation = base + 5 + key_.lane;
        m.points = base + ((s == 5 || s == 7) ? 6 : 7) + 8 * key_.lane;
    }
    return m;
}
double AdiShaperPanel::read(int id) const {
    if (!binding_.read || id <= 0 || id > binding_.parameterCount)
        return 0;
    const double value = binding_.read(id);
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}
bool AdiShaperPanel::freeCurve() const { return binding_.wave && binding_.editWave; }
const std::vector<Panel::Point> &AdiShaperPanel::points() const {
    if (drag_ == Drag::Node || drag_ == Drag::Stroke || drag_ == Drag::Bend || drag_ == Drag::Level)
        return working_;
    if (!pointsDirty_)
        return cachedPoints_;
    pointsDirty_ = false;
    cachedPoints_.clear();
    if (freeCurve())
        cachedPoints_ = binding_.wave(key_);
    if (cachedPoints_.empty()) {
        const auto m = mapping();
        const auto shape = read(m.interpolation) < .5 ? Shape::Hold : Shape::Line;
        for (int i = 0; i < 8; ++i)
            cachedPoints_.push_back({i / 8.f, static_cast<float>(read(m.points + i)), 0.f, shape});
    }
    return cachedPoints_;
}
float AdiShaperPanel::average(const std::vector<Point> &nodes) {
    if (nodes.empty())
        return 0;
    float area = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto &a = nodes[i];
        const Point b =
            i + 1 < nodes.size() ? nodes[i + 1] : Point{1 + nodes.front().x, nodes.front().y, 0, Shape::Line};
        const float mid = a.shape == Shape::Hold ? 0.f : (.5f - (a.shape == Shape::Bend ? a.bend : 0.f) / 6.f);
        area += (b.x - a.x) * (a.y + (b.y - a.y) * mid);
    }
    return clamp01(area);
}
AdiShaperPanel::Rect AdiShaperPanel::graph() const { return {52, 217, 935, 262}; }
AdiShaperPanel::Rect AdiShaperPanel::curveBounds() const {
    return graph().transformedBy(
        juce::AffineTransform::scale(getWidth() / designWidth, getHeight() / designHeight));
}
AdiShaperPanel::Pos AdiShaperPanel::logical(Pos p) const {
    return {p.x * designWidth / std::max(1, getWidth()), p.y * designHeight / std::max(1, getHeight())};
}
AdiShaperPanel::Pos AdiShaperPanel::toPlot(Pos p) const {
    const auto r = graph();
    return {clamp01((p.x - r.getX()) / r.getWidth()), clamp01((r.getBottom() - p.y) / r.getHeight())};
}
std::vector<AdiShaperPanel::Control> AdiShaperPanel::controls() const {
    const auto m = mapping();
    std::vector<Control> v;
    v.push_back({{628, 6, 76, 30}, 1, "Master Mix", percent(read(1)), 1});
    v.push_back({{923, 96, 66, 96}, m.mix, "Mix", percent(read(m.mix)), 1});
    const bool hz = read(m.clock) > .5;
    const auto &curve = points();
    const auto current =
        curve.empty()
            ? Shape::Hold
            : curve[static_cast<std::size_t>(std::clamp(selected_, 0, static_cast<int>(curve.size()) - 1))]
                  .shape;
    v.push_back({{71, 491, 119, 27},
                 hz ? m.rate : m.cycle,
                 hz ? "Rate" : "Cycle",
                 hz ? frequency(.02 * std::pow(262000., read(m.rate))) : cycle(read(m.cycle)),
                 2});
    v.push_back({{195, 491, 79, 27}, m.clock, "", hz ? "Hz" : "Sync", 3});
    v.push_back({{282, 491, 89, 27},
                 m.interpolation,
                 "",
                 current == Shape::Hold   ? "Hold"
                 : current == Shape::Bend ? "Bend"
                                          : "Linear",
                 3});
    if (m.on)
        v.push_back({{570, 97, 67, 35}, m.on, "", read(m.on) > .5 ? "Enabled" : "Disabled", 3});
    if (m.type) {
        static constexpr std::array<const char *, 5> filters{"LP 6", "LP 12", "HP 6", "HP 12", "BP 12"};
        static constexpr std::array<const char *, 6> sweeps{"Off",      "Flange",    "4 stages",
                                                            "8 stages", "12 stages", "16 stages"};
        static constexpr std::array<const char *, 3> drives{"Saturate", "Clip", "Rectify"};
        static constexpr std::array<const char *, 2> dither{"Off", "On"};
        const std::span<const char *const> choices = key_.stage == 7 ? std::span<const char *const>(timeRanges)
                                                     : key_.stage == 3 ? std::span<const char *const>(filters)
                                                     : key_.stage == 4 ? std::span<const char *const>(sweeps)
                                                     : key_.stage == 5 ? std::span<const char *const>(drives)
                                                                       : std::span<const char *const>(dither);
        const int n = static_cast<int>(choices.size());
        juce::String val = choices[static_cast<std::size_t>(juce::roundToInt(read(m.type) * (n - 1)))];
        v.push_back({{348, 96, 84, 96},
                     m.type,
                     key_.stage == 7   ? "Time range"
                     : key_.stage == 6 ? "Dither"
                                       : "Mode",
                     val,
                     10 + n});
    }
    if (key_.stage == 4) {
        const int polarity = Core::SweepLowPolarity + key_.band + 1;
        v.push_back({{718, 149, 89, 31}, polarity, "", read(polarity) < .5 ? "Positive" : "Negative", 3});
    } else if (key_.stage == 5) {
        const auto band = static_cast<Core::Band>(key_.band);
        const int input = Core::driveParam(band, Core::InputTrim) + 1,
                  output = Core::driveParam(band, Core::OutputTrim) + 1;
        for (auto id : {input, output}) {
            const double norm = read(id) - .5;
            const double gain = norm * 6.020599913 * ((id == input) == (norm >= 0) ? 12 : 4);
            v.push_back({{id == input ? 650.f : 745.f, 98, 85, 44},
                         id,
                         id == input ? "Input trim" : "Output trim",
                         juce::String(gain, 1) + " dB",
                         4});
        }
    }
    const int layout = juce::roundToInt(read(m.layout) * 3);
    for (int edge = 0; edge < 2; ++edge) {
        if (!(layout & (1 << edge)))
            continue;
        const int id = key_.stage < 3 ? Core::VolumeLowSlope + 1 + key_.stage + 3 * edge : m.on + 4 + edge;
        const int db = 6 << juce::roundToInt(read(id) * 2);
        v.push_back({{15.f + edge * 142.f, 173, 127, 17}, id, "", juce::String(db) + " dB/oct", 13});
    }
    return v;
}
void AdiShaperPanel::setSpectrum(std::span<const float> db) {
    auto v = db.first(std::min(db.size(), std::size_t{1024}));
    spectrum_.assign(v.begin(), v.end());
    repaint();
}
namespace {
juce::String db(float gain) {
    return gain > 0 ? juce::String(20.f * std::log10(gain), 1) + " dB" : juce::String::fromUTF8("-inf");
}
void lfoIcon(juce::Graphics &g, Rect r, int stage, juce::Colour c) {
    juce::Path p;
    for (int i = 0; i <= 28; ++i) {
        const float t = i / 28.f;
        const float v = std::sin((1.0f + .12f * stage) * std::numbers::pi_v<float> * 2 * t + .6f);
        const Pos pos{r.getX() + t * r.getWidth(), r.getCentreY() + r.getHeight() * .38f * v};
        if (!i)
            p.startNewSubPath(pos);
        else
            p.lineTo(pos);
    }
    g.setColour(c);
    g.strokePath(p, juce::PathStrokeType(1.8f));
}
} // namespace
void AdiShaperPanel::paint(juce::Graphics &g) {
    const auto accent = colour(key_.stage);
    g.addTransform(juce::AffineTransform::scale(getWidth() / designWidth, getHeight() / designHeight));
    g.fillAll(juce::Colour(0xff202025));
    g.setColour(juce::Colour(0xff37373b));
    g.fillRect(Rect(0, 0, 1000, 42));
    label(g, "ADI-Shaper", {14, 0, 245, 42}, 25, juce::Colour(0xffffab46), juce::Justification::centredLeft);
    box(g, {373, 6, 190, 30}, plate);
    label(g, binding_.presetName, {382, 6, 174, 30}, 15, ink, juce::Justification::centredLeft);
    float tabX = 3;
    for (int s = 0; s < 8; ++s)
        if (tabs_[static_cast<std::size_t>(s)]) {
            const Rect tab(tabX, 45, 78, 42);
            box(g, tab, s == key_.stage ? juce::Colour(0xff303036) : plate);
            lfoIcon(g, {tabX + 23, 48, 30, 15}, s, colour(s));
            label(g, names[static_cast<std::size_t>(s)], {tabX, 65, 78, 18}, 12, colour(s));
            tabX += 80;
        }
    label(g, "+", {tabX, 44, 49, 41}, 25, dim);
    g.setColour(juce::Colour(0xff303035));
    g.fillRect(Rect(0, 87, 1000, 115));
    const auto m = mapping();
    const int layout = juce::roundToInt(read(m.layout) * 3);
    const Rect bandBox(9, 96, 328, 96);
    box(g, bandBox, plate);
    const bool hasLow = (layout & 1) != 0, hasHigh = (layout & 2) != 0;
    const float lo = hasLow ? static_cast<float>(read(m.low)) : 0.f,
                hi = hasHigh ? static_cast<float>(read(m.high)) : 1.f;
    const std::array<float, 4> edges{0, lo, std::max(lo, hi), 1};
    for (int b = 0; b < 3; ++b) {
        auto x = edges[static_cast<std::size_t>(b)], end = edges[static_cast<std::size_t>(b + 1)];
        if (end <= x)
            continue;
        const Rect r(bandBox.getX() + bandBox.getWidth() * x, 99, bandBox.getWidth() * (end - x), 72);
        if (b == key_.band) {
            g.setColour(accent.withAlpha(.055f));
            g.fillRect(r);
        }
        label(g, bandNames[static_cast<std::size_t>(b)], r.reduced(5, 3), 11, dim,
              juce::Justification::topLeft);
        g.setColour(b == key_.band ? accent : dim);
        g.fillRect(r.getX(), 169.f, r.getWidth(), 3.f);
    }
    if (spectrum_.size() > 1) {
        juce::Path p;
        for (std::size_t i = 0; i < spectrum_.size(); ++i) {
            const float x = 10.f + 326.f * static_cast<float>(i) / static_cast<float>(spectrum_.size() - 1);
            const float y = 167.f - 51.f * clamp01((spectrum_[i] + 72.f) / 72.f);
            if (i == 0)
                p.startNewSubPath(x, y);
            else
                p.lineTo(x, y);
        }
        g.setColour(ink.withAlpha(.65f));
        g.strokePath(p, juce::PathStrokeType(1));
    }
    for (auto b : {0, 1})
        if (b == 0 ? hasLow : hasHigh) {
            const float x = 9 + 328 * (b == 0 ? lo : hi);
            g.setColour(dim);
            g.drawLine(x, 97, x, 172, 1);
            g.fillEllipse(x - 3, 167, 6, 6);
        }
    if (!layout)
        label(g, "Bands", {92, 173, 146, 17}, 11, dim);
    label(g, "+", {308, 173, 27, 17}, 15, ink);
    const auto &nodes = points();
    if (!m.type) {
        box(g, {348, 96, 74, 96}, plate);
        label(g, names[static_cast<std::size_t>(key_.stage)], {348, 98, 74, 21}, 12, dim);
        const float mean = average(nodes);
        g.setColour(rail);
        g.fillRoundedRectangle(383, 121, 5, 44, 2);
        g.setColour(accent);
        g.fillRoundedRectangle(383, 165 - 44 * mean, 5, std::max(1.f, 44 * mean), 2);
        label(g,
              key_.stage == 1   ? percent(2. * mean)
              : key_.stage == 2 ? percent(2. * mean - 1)
                                : percent(mean),
              {348, 168, 74, 18}, 14, ink);
    }
    box(g, {440, 96, 115, 96}, plate);
    label(g, "Output", {444, 98, 107, 21}, 12, dim);
    for (int i = 0; i < 2; ++i) {
        const float v = peaks_[static_cast<std::size_t>(i)], y = 129.f + i * 20;
        label(g, i ? "R" : "L", {443, y - 8, 12, 16}, 10, dim);
        g.setColour(rail);
        g.fillRect(459.f, y, 45.f, 3.f);
        g.setColour(v > 1 ? juce::Colours::orangered : accent);
        g.fillRect(459.f, y, 45.f * clamp01((v > 0 ? 20.f * std::log10(v) + 60.f : 0.f) / 60.f), 3.f);
        label(g, db(v), {506, y - 8, 45, 16}, 10, ink);
    }
    if (key_.stage == 3 || key_.stage == 4 || key_.stage == 6) {
        const std::array<const char *, 2> text = key_.stage == 3   ? std::array{"Cutoff", "Resonance"}
                                                 : key_.stage == 4 ? std::array{"Center", "Feedback"}
                                                                   : std::array{"Bits", "Rate"};
        for (int i = 0; i < 2; ++i) {
            const Rect r(570.f + 73.f * i, 149, 69, 31);
            box(g, r, plate);
            label(g, text[static_cast<std::size_t>(i)], r, 12, i == key_.lane ? accent : dim);
        }
    }
    const auto plot = graph();
    g.setColour(juce::Colour(0xff111114));
    g.fillRect(Rect(0, 202, 1000, 320));
    for (int i = 0; i <= 64; ++i) {
        const float x = plot.getX() + plot.getWidth() * i / 64.f;
        g.setColour(juce::Colour(i % 16 == 0 ? 0xff3e3e43 : (i % 4 == 0 ? 0xff29292e : 0xff1b1b1e)));
        g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
        if (i % 16 == 0)
            label(g, i == 0 ? "0" : juce::String(i / 16) + "/4", {x - 15, 203, 30, 14}, 10, dim);
    }
    for (int i = 0; i <= 8; ++i) {
        const float y = plot.getY() + i * plot.getHeight() / 8;
        g.setColour(juce::Colour(i % 4 == 0 ? 0xff3d3d43 : 0xff242427));
        g.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
    }
    auto envelope = [&](std::size_t slot, juce::Colour c) {
        const auto count = std::min(traces_[slot].size(), traces_[slot + 1].size());
        if (count < 2)
            return;
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(plot.toNearestInt());
        juce::Path wave;
        auto pos = [&](std::size_t i, float v) {
            return Pos{plot.getX() + plot.getWidth() * static_cast<float>(i) / static_cast<float>(count - 1),
                       plot.getCentreY() - .47f * plot.getHeight() * std::clamp(v, -1.2f, 1.2f)};
        };
        wave.startNewSubPath(pos(0, traces_[slot + 1][0]));
        for (std::size_t i = 1; i < count; ++i)
            wave.lineTo(pos(i, traces_[slot + 1][i]));
        for (auto i = count; i-- > 0;)
            wave.lineTo(pos(i, traces_[slot][i]));
        wave.closeSubPath();
        g.setColour(c);
        g.fillPath(wave);
    };
    if (showInput_)
        envelope(0, ink.withAlpha(.19f));
    if (showOutput_)
        envelope(2, accent.withAlpha(.22f));
    if (phase_ > 0 && phase_ < 1) {
        const auto x = plot.getX() + phase_ * plot.getWidth();
        g.setColour(accent.withAlpha(.3f));
        g.drawLine(x, plot.getY(), x, plot.getBottom());
    }
    auto dot = [&](float x, float y) {
        return Pos{plot.getX() + plot.getWidth() * x, plot.getBottom() - plot.getHeight() * y};
    };
    if (!nodes.empty()) {
        juce::Path curve;
        auto segmentTo = [&](Point a, Point b) {
            if (a.shape == Shape::Hold)
                curve.lineTo(dot(b.x, a.y));
            if (a.shape == Shape::Bend && b.x > a.x) {
                const float controlY = a.y + .5f * (1.f - std::clamp(a.bend, -1.f, 1.f)) * (b.y - a.y);
                curve.quadraticTo(dot(.5f * (a.x + b.x), controlY), dot(b.x, b.y));
            } else
                curve.lineTo(dot(b.x, b.y));
        };
        auto left = nodes.back();
        left.x -= 1;
        curve.startNewSubPath(dot(left.x, left.y));
        for (const auto &right : nodes) {
            segmentTo(left, right);
            left = right;
        }
        auto repeat = nodes.front();
        repeat.x += 1;
        segmentTo(left, repeat);
        {
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(plot.expanded(1, 1).toNearestInt());
            g.setColour(accent);
            g.strokePath(curve, juce::PathStrokeType(1.9f));
        }
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto &a = nodes[i];
            const auto &prev = nodes[i ? i - 1 : nodes.size() - 1];
            if (i > 0 && a.y == prev.y && a.shape == Shape::Hold && static_cast<int>(i) != selected_)
                continue;
            const auto xy = dot(a.x, a.y);
            if (a.shape == Shape::Hold && a.y != prev.y) {
                g.setColour(ink);
                const auto p = dot(a.x, prev.y);
                g.fillEllipse(p.x - 2.7f, p.y - 2.7f, 5.4f, 5.4f);
            }
            g.setColour(static_cast<int>(i) == selected_ ? accent : ink);
            g.fillEllipse(xy.x - 3, xy.y - 3, 6, 6);
        }
    }
    std::array<juce::String, 3> scale{"100%", "50%", "0%"};
    if (key_.stage == 0)
        scale = {"0 dB", "-6", "-inf"};
    else if (key_.stage == 1)
        scale = {"200%", "100%", "0%"};
    else if (key_.stage == 2)
        scale = {"R", "C", "L"};
    else if ((key_.stage == 3 || key_.stage == 4) && key_.lane == 0)
        scale = {"21.1k", "659", "20.6"};
    else if (key_.stage == 7) {
        const int range = juce::roundToInt(read(m.type) * 7);
        const bool ms = range >= 5 || (!range && read(m.clock) > .5);
        const double extent = range >= 5
                                  ? std::array<double, 3>{2.5, 10, 20}[static_cast<std::size_t>(range - 5)]
                              : range ? std::exp2(range + 1.)
                              : ms    ? 1000. / (.02 * std::pow(262000., read(m.rate)))
                                      : std::exp2(read(m.cycle) * 12. - 5.);
        auto unit = [&](double v) {
            return juce::String(-v, std::abs(v - std::round(v)) < .001 ? 0 : 2) + (ms ? " ms" : "/4");
        };
        scale = {"0", unit(extent * .5), unit(extent)};
    }
    label(g, scale[0], {1, plot.getY() - 7, 43, 14}, 10, dim, juce::Justification::centredRight);
    label(g, scale[1], {23, plot.getCentreY() - 7, 20, 14}, 9, dim, juce::Justification::centredRight);
    label(g, scale[2], {1, plot.getBottom() - 7, 43, 14}, 10, dim, juce::Justification::centredRight);
    const std::array<const char *, 5> tools{"", "", "#", "In", "Out"};
    for (int i = 0; i < 5; ++i) {
        auto r = button(i);
        box(g, r, plate);
        bool active = i == 0   ? tool_ == Tool::Edit
                      : i == 1 ? tool_ == Tool::Draw
                      : i == 2 ? snap_
                      : i == 3 ? showInput_
                               : showOutput_;
        g.setColour(active ? accent : dim);
        const auto c = r.getCentre();
        if (i == 0) {
            juce::Path arrow;
            arrow.startNewSubPath(c.x - 5, c.y - 7);
            arrow.lineTo(c.x + 5, c.y + 1);
            arrow.lineTo(c.x + 1, c.y + 2);
            arrow.lineTo(c.x + 4, c.y + 7);
            arrow.lineTo(c.x + 1, c.y + 8);
            arrow.lineTo(c.x - 2, c.y + 3);
            arrow.lineTo(c.x - 5, c.y + 6);
            arrow.closeSubPath();
            g.fillPath(arrow);
        } else if (i == 1) {
            g.drawLine(c.x - 5, c.y + 5, c.x + 5, c.y - 5, 2.5f);
            g.drawLine(c.x - 6, c.y + 7, c.x - 4, c.y + 6, 1.f);
        } else {
            label(g, tools[static_cast<std::size_t>(i)], r, 13, active ? accent : dim);
        }
    }
    for (const auto &c : controls()) {
        box(g, c.r, plate);
        if (c.mode == 1 && c.r.getHeight() > 40) {
            label(g, c.title, c.r.withHeight(20), 12, dim);
            const float v = static_cast<float>(read(c.id)), x = c.r.getCentreX(), y = c.r.getY() + 23;
            g.setColour(rail);
            g.fillRoundedRectangle(x - 3, y, 6, 48, 3);
            g.setColour(accent);
            g.fillRoundedRectangle(x - 3, y + 48 * (1 - v), 6, std::max(1.f, 48 * v), 3);
            g.setColour(ink);
            g.fillEllipse(x - 4, y + 48 * (1 - v) - 4, 8, 8);
            label(g, c.text, c.r.withTop(c.r.getBottom() - 23), 14, ink);
        } else if ((c.mode == 1 || c.mode == 4 || c.mode >= 10) && c.title.isNotEmpty()) {
            label(g, c.title, c.r.withHeight(c.r.getHeight() * .48f), 11, dim);
            label(g, c.text, c.r.withTrimmedTop(c.r.getHeight() * .45f), 13, ink);
        } else
            label(g, c.text, c.r, 13, c.mode == 3 ? accent : ink);
    }
    if (freeCurve()) {
        box(g, {378, 491, 70, 27}, plate);
        label(g, "Bend", {378, 491, 70, 27}, 12, segmentShape_ == Shape::Bend ? accent : dim);
    }
    label(g, "Wave shapes", {10, 523, 140, 23}, 13, accent, juce::Justification::centredLeft);
    for (int i = 0; i < 5; ++i) {
        auto r = palette(i);
        box(g, r, plate);
        juce::Path p;
        for (int j = 0; j < 36; ++j) {
            const float x = j / 35.f;
            const float h = i == 0   ? .9f
                            : i == 1 ? (x < .5f ? .1f : .9f)
                            : i == 2 ? x
                            : i == 3 ? 1 - x
                                     : 1 - std::abs(2 * x - 1);
            const Pos a{r.getX() + 4 + x * (r.getWidth() - 8), r.getBottom() - 4 - h * (r.getHeight() - 8)};
            if (!j)
                p.startNewSubPath(a);
            else
                p.lineTo(a);
        }
        g.setColour(ink);
        g.strokePath(p, juce::PathStrokeType(1.35f));
    }
    box(g, {0, 578, 1000, 22}, juce::Colour(0xff37373b), 0);
    label(g,
          tool_ == Tool::Draw
              ? "Draw across the grid to shape the curve."
              : (freeCurve()
                     ? "Drag a point to move it. Double-click the grid to add a point. Alt-drag a segment to "
                       "bend it."
                     : "Drag a point to change its height. Use the arrow keys to select and move a point."),
          {12, 578, 970, 22}, 11, ink, juce::Justification::centredLeft);
}
bool AdiShaperPanel::send(Phase p, std::span<const Change> values) {
    return binding_.parameters && binding_.parameters(p, values);
}
void AdiShaperPanel::setOnce(int id, double v) {
    if (id <= 0 || id > binding_.parameterCount)
        return;
    const std::array<Change, 1> before{{{id, read(id)}}}, after{{{id, std::clamp(v, 0.0, 1.0)}}};
    if (before[0].value == after[0].value || !send(Phase::Begin, before))
        return;
    if (send(Phase::Change, after))
        send(Phase::End, after);
    else
        send(Phase::Cancel, before);
    refresh();
}
bool AdiShaperPanel::beginParam(int id) {
    if (id <= 0 || id > binding_.parameterCount)
        return false;
    originalValue_ = read(id);
    originalParams_ = {{id, originalValue_}};
    edits_ = originalParams_;
    if (!send(Phase::Begin, originalParams_))
        return false;
    activeId_ = id;
    drag_ = Drag::Control;
    return true;
}
void AdiShaperPanel::startNodes(Drag kind) {
    original_ = points();
    working_ = original_;
    flexible_ = freeCurve();
    if (flexible_) {
        if (!binding_.editWave(key_, Phase::Begin, original_))
            return;
    } else {
        originalParams_.clear();
        const auto m = mapping();
        for (int i = 0; i < 8; ++i)
            originalParams_.push_back({m.points + i, original_[static_cast<std::size_t>(i)].y});
        originalParams_.push_back({m.interpolation, read(m.interpolation)});
        if (!send(Phase::Begin, originalParams_))
            return;
        edits_ = originalParams_;
    }
    drag_ = kind;
}
void AdiShaperPanel::updateNodes(std::vector<Point> pts) {
    bool ok = false;
    if (flexible_)
        ok = binding_.editWave(key_, Phase::Change, pts);
    else if (pts.size() == 8) {
        const auto m = mapping();
        edits_.clear();
        for (int i = 0; i < 8; ++i)
            edits_.push_back({m.points + i, pts[static_cast<std::size_t>(i)].y});
        edits_.push_back({m.interpolation, pts.front().shape == Shape::Hold ? 0. : 1.});
        ok = send(Phase::Change, edits_);
    }
    if (ok)
        working_ = std::move(pts);
    else
        cancel();
    repaint();
}
void AdiShaperPanel::finish(Phase phase) {
    const auto old = drag_;
    if (old == Drag::None)
        return;
    drag_ = Drag::None;
    if (old != Drag::Control && flexible_)
        binding_.editWave(key_, phase, phase == Phase::Cancel ? original_ : working_);
    else
        send(phase, phase == Phase::Cancel ? originalParams_ : edits_);
    activeId_ = 0;
    original_.clear();
    working_.clear();
    originalParams_.clear();
    edits_.clear();
    refresh();
}
void AdiShaperPanel::cancel() { finish(Phase::Cancel); }
void AdiShaperPanel::focusLost(FocusChangeType) { cancel(); }
void AdiShaperPanel::visibilityChanged() {
    if (!isShowing())
        cancel();
}
void AdiShaperPanel::enablementChanged() {
    if (!isEnabled())
        cancel();
}
int AdiShaperPanel::nearestNode(Pos p, bool nearOnly) const {
    const auto r = graph();
    float best = std::numeric_limits<float>::max();
    int idx = -1;
    const auto &v = points();
    for (std::size_t i = 0; i < v.size(); ++i) {
        const Pos q{r.getX() + v[i].x * r.getWidth(), r.getBottom() - v[i].y * r.getHeight()};
        const float delta = nearOnly ? p.getDistanceFrom(q) : std::abs(p.x - q.x);
        if (delta < best) {
            idx = static_cast<int>(i);
            best = delta;
        }
    }
    return nearOnly && best > 9.f ? -1 : idx;
}
int AdiShaperPanel::segment(Pos p) const {
    const auto &v = points();
    const auto x = toPlot(p).x;
    auto it = std::upper_bound(v.begin(), v.end(), x, [](float a, const Point &b) { return a < b.x; });
    return it == v.begin() ? static_cast<int>(v.size()) - 1 : static_cast<int>(it - v.begin()) - 1;
}
void AdiShaperPanel::stroke(Pos p) {
    const auto a = toPlot(previous_), b = toPlot(p);
    auto v = working_;
    if (v.empty())
        return;
    if (flexible_) {
        const float from = snap_ ? std::round(a.x * 32.f) / 32.f : a.x;
        const float q = snap_ ? std::round(b.x * 32.f) / 32.f : b.x;
        std::erase_if(v, [&](const Point &pt) {
            return pt.x > 0 && pt.x < 1 && pt.x > std::min(from, q) && pt.x < std::max(from, q);
        });
        auto it = std::lower_bound(v.begin(), v.end(), q, [](const Point &pt, float x) { return pt.x < x; });
        if (it != v.end() && std::abs(it->x - q) < .0001f)
            it->y = b.y;
        else if (v.size() < 512)
            v.insert(it, {q, b.y, 0, segmentShape_});
    } else {
        const auto index = [](float x) { return std::clamp(juce::roundToInt(x * 8), 0, 7); };
        const int lo = std::min(index(a.x), index(b.x)), hi = std::max(index(a.x), index(b.x));
        for (int i = lo; i <= hi; ++i) {
            const float t = a.x == b.x ? 1.f : clamp01((i / 8.f - a.x) / (b.x - a.x));
            v[static_cast<std::size_t>(i)].y = a.y + t * (b.y - a.y);
        }
        v[static_cast<std::size_t>(index(b.x))].y = b.y;
    }
    updateNodes(std::move(v));
    previous_ = p;
}
void AdiShaperPanel::controlDrag(Pos p) {
    const auto m = mapping();
    double value = originalValue_ + (down_.y - p.y) / 110.f;
    if (activeId_ == m.low || activeId_ == m.high) {
        value = clamp01((p.x - 9.f) / 328.f);
        if (juce::roundToInt(read(m.layout) * 3) == 3)
            value = activeId_ == m.low ? std::min(value, read(m.high)) : std::max(value, read(m.low));
    }
    edits_[0].value = std::clamp(value, 0.0, 1.0);
    if (!send(Phase::Change, edits_))
        cancel();
    repaint();
}
void AdiShaperPanel::updatePointer(Pos p) {
    if (drag_ == Drag::Control) {
        controlDrag(p);
        return;
    }
    if (drag_ == Drag::Stroke) {
        stroke(p);
        return;
    }
    auto v = working_;
    if (drag_ == Drag::Level) {
        const float before = average(original_);
        const float target = clamp01(before + (down_.y - p.y) / 96.f);
        for (std::size_t i = 0; i < v.size(); ++i) {
            const float y = original_[i].y;
            if (target > before && before < 1.f)
                v[i].y = y + (1.f - y) * (target - before) / (1.f - before);
            else if (before > 0.f)
                v[i].y = y * target / before;
            else
                v[i].y = target;
        }
        updateNodes(std::move(v));
        return;
    }
    if (drag_ == Drag::Bend && selectedSegment_ >= 0 && selectedSegment_ < static_cast<int>(v.size())) {
        const auto i = static_cast<std::size_t>(selectedSegment_);
        v[i].shape = Shape::Bend;
        v[i].bend = std::clamp(original_[i].bend + (down_.y - p.y) / 80.f, -1.f, 1.f);
        updateNodes(std::move(v));
        return;
    }
    if (drag_ != Drag::Node || selected_ < 0 || selected_ >= static_cast<int>(v.size()))
        return;
    const auto i = static_cast<std::size_t>(selected_);
    auto q = toPlot(p);
    v[i].y = q.y;
    if (flexible_) {
        float x = snap_ ? std::round(q.x * 32.f) / 32.f : q.x;
        v[i].x = std::clamp(x, i ? v[i - 1].x : 0.f, (i + 1 < v.size()) ? v[i + 1].x : 1.f);
    }
    updateNodes(std::move(v));
}
void AdiShaperPanel::chooseShape(Shape shape) {
    segmentShape_ = shape;
    if (!freeCurve()) {
        setOnce(mapping().interpolation, shape == Shape::Hold ? 0 : 1);
        return;
    }
    startNodes(Drag::Node);
    if (drag_ == Drag::None)
        return;
    auto v = working_;
    if (v.empty()) {
        cancel();
        return;
    }
    auto &pt = v[static_cast<std::size_t>(std::clamp(selected_, 0, static_cast<int>(v.size()) - 1))];
    pt.shape = shape;
    if (shape == Shape::Bend && pt.bend == 0)
        pt.bend = .5f;
    updateNodes(std::move(v));
    finish(Phase::End);
}
void AdiShaperPanel::writeShape(int shape) {
    startNodes(Drag::Stroke);
    if (drag_ == Drag::None)
        return;
    auto v = working_;
    auto form = [shape](float x) {
        return shape == 0   ? 1.f
               : shape == 1 ? (x < .5f ? 0.f : 1.f)
               : shape == 2 ? x
               : shape == 3 ? 1.f - x
                            : 1.f - std::abs(2 * x - 1);
    };
    const auto mode = shape <= 1 ? Shape::Hold : Shape::Line;
    if (flexible_) {
        v = {{0, form(0), 0, mode}};
        if (shape == 1 || shape == 4)
            v.push_back({.5f, form(.5f), 0, mode});
        v.push_back({1, form(1), 0, mode});
    } else
        for (auto &pt : v) {
            pt.y = form(pt.x);
            pt.shape = mode;
        }
    updateNodes(std::move(v));
    finish(Phase::End);
}
void AdiShaperPanel::showStages(Pos) {
    juce::PopupMenu menu;
    for (int s = 0; s < (binding_.parameterCount >= 463 ? 8 : 7); ++s)
        menu.addItem(s + 1, names[static_cast<std::size_t>(s)], true, s == key_.stage);
    juce::Component::SafePointer<AdiShaperPanel> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this), [safe](int choice) {
        if (safe && choice) {
            safe->select({choice - 1, 1, 0});
            const auto id = safe->mapping().on;
            if (id)
                safe->setOnce(id, 1);
        }
    });
}
void AdiShaperPanel::mouseDown(const juce::MouseEvent &e) {
    if (!isEnabled())
        return;
    cancel();
    grabKeyboardFocus();
    const auto p = logical(e.position);
    previous_ = down_ = p;
    if (inside({373, 6, 190, 30}, p) && binding_.choosePreset) {
        binding_.choosePreset();
        return;
    }
    float tabX = 3;
    for (int s = 0; s < 8; ++s)
        if (tabs_[static_cast<std::size_t>(s)]) {
            if (inside({tabX, 45, 78, 42}, p)) {
                select({s, key_.band, 0});
                return;
            }
            tabX += 80;
        }
    if (inside({tabX, 44, 49, 41}, p)) {
        showStages(p);
        return;
    }
    for (int i = 0; i < 5; ++i)
        if (button(i).contains(p)) {
            if (i < 2)
                tool_ = i ? Tool::Draw : Tool::Edit;
            else if (i == 2)
                snap_ = !snap_;
            else if (i == 3)
                showInput_ = !showInput_;
            else
                showOutput_ = !showOutput_;
            repaint();
            return;
        }
    for (int i = 0; i < 5; ++i)
        if (palette(i).contains(p)) {
            writeShape(i);
            return;
        }
    const auto m = mapping();
    if (!m.type && inside({348, 96, 74, 96}, p)) {
        startNodes(Drag::Level);
        return;
    }
    if (inside({307, 172, 29, 21}, p)) {
        const int layout = (juce::roundToInt(read(m.layout) * 3) + 1) % 4;
        setOnce(m.layout, layout / 3.);
        select({key_.stage, 1, key_.lane});
        return;
    }
    if (inside({9, 96, 328, 77}, p)) {
        const int layout = juce::roundToInt(read(m.layout) * 3);
        const double norm = (p.x - 9.f) / 328.f;
        if ((layout & 1) && std::abs(norm - read(m.low)) < .023) {
            beginParam(m.low);
            return;
        }
        if ((layout & 2) && std::abs(norm - read(m.high)) < .023) {
            beginParam(m.high);
            return;
        }
        const int band = (layout & 1) && norm < read(m.low) ? 0 : (layout & 2) && norm > read(m.high) ? 2 : 1;
        select({key_.stage, band, key_.lane});
        return;
    }
    if ((key_.stage == 3 || key_.stage == 4 || key_.stage == 6) && inside({570, 149, 142, 31}, p)) {
        select({key_.stage, key_.band, p.x < 640 ? 0 : 1});
        return;
    }
    if (freeCurve() && inside({378, 491, 70, 27}, p)) {
        chooseShape(Shape::Bend);
        return;
    }
    for (const auto &c : controls())
        if (c.r.contains(p)) {
            if (c.id == m.interpolation)
                chooseShape(c.text == "Hold" ? Shape::Line : Shape::Hold);
            else if (c.mode == 3)
                setOnce(c.id, read(c.id) < .5 ? 1 : 0);
            else if (c.mode >= 10) {
                int n = c.mode - 10;
                setOnce(c.id, ((juce::roundToInt(read(c.id) * (n - 1)) + 1) % n) / static_cast<double>(n - 1));
            } else
                beginParam(c.id);
            return;
        }
    if (!graph().expanded(4).contains(p) || e.getNumberOfClicks() > 1)
        return;
    if (tool_ == Tool::Draw) {
        startNodes(Drag::Stroke);
        if (drag_ != Drag::None)
            stroke(p);
        return;
    }
    if (e.mods.isAltDown() && freeCurve()) {
        selectedSegment_ = segment(p);
        startNodes(Drag::Bend);
        return;
    }
    selected_ = nearestNode(p, freeCurve());
    if (selected_ >= 0)
        startNodes(Drag::Node);
    repaint();
}
void AdiShaperPanel::mouseDrag(const juce::MouseEvent &e) {
    if (drag_ != Drag::None)
        updatePointer(logical(e.position));
}
void AdiShaperPanel::mouseUp(const juce::MouseEvent &) { finish(Phase::End); }
void AdiShaperPanel::mouseDoubleClick(const juce::MouseEvent &e) {
    const auto p = logical(e.position);
    if (!freeCurve() || tool_ != Tool::Edit || !graph().contains(p))
        return;
    cancel();
    const int nearest = nearestNode(p, true);
    startNodes(Drag::Node);
    if (drag_ == Drag::None)
        return;
    auto v = working_;
    if (nearest > 0 && v.size() > 1) {
        v.erase(v.begin() + nearest);
        selected_ = -1;
    } else if (nearest < 0 && v.size() < 512) {
        auto q = toPlot(p);
        if (snap_)
            q.x = std::round(q.x * 32.f) / 32.f;
        auto it = std::upper_bound(v.begin(), v.end(), q.x, [](float x, const Point &a) { return x < a.x; });
        selected_ = static_cast<int>(it - v.begin());
        v.insert(it, {q.x, q.y, 0, segmentShape_});
    } else {
        cancel();
        return;
    }
    updateNodes(std::move(v));
    finish(Phase::End);
}
bool AdiShaperPanel::keyPressed(const juce::KeyPress &key) {
    if (key == juce::KeyPress::escapeKey && drag_ != Drag::None) {
        cancel();
        return true;
    }
    if (drag_ != Drag::None)
        return false;
    const auto &v = points();
    if (v.empty())
        return false;
    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey) {
        selected_ = std::clamp(selected_ + (key == juce::KeyPress::rightKey ? 1 : -1), 0,
                               static_cast<int>(v.size()) - 1);
        repaint();
        return true;
    }
    if (selected_ < 0 || selected_ >= static_cast<int>(v.size()))
        return false;
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey) {
        startNodes(Drag::Node);
        if (drag_ == Drag::None)
            return false;
        auto edit = v;
        auto &y = edit[static_cast<std::size_t>(selected_)].y;
        y = clamp01(y + (key == juce::KeyPress::upKey ? .01f : -.01f));
        updateNodes(std::move(edit));
        finish(Phase::End);
        return true;
    }
    return false;
}
} // namespace adi::ui
