// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/adi_shaper_panel.hpp"
#include "adi/dsp/adi_shaper.hpp"
#include "juce/adi_shaper_spectrum.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <stdexcept>

namespace {
using Panel = adi::ui::AdiShaperPanel;
using Core = adi::dsp::AdiShaper;
void expect(bool value, const char *what) {
    if (!value)
        throw std::runtime_error(what);
}
struct Fixture {
    std::array<double, 464> values{}, baseline{};
    std::vector<Panel::Point> wave, before;
    Core dsp;
    int starts = 0, changes = 0, ends = 0, cancels = 0, presets = 0;
    Fixture() {
        values[1] = 1;
        values[3] = 1. / 3;
        values[4] = 2. / 3;
        values[18] = 1;
        values[20] = 7. / 12;
        for (int i = 23; i <= 30; ++i) {
            values[static_cast<std::size_t>(i)] = i >= 27 ? 1 : 0;
            dsp.set(static_cast<Core::Param>(i - 1), i >= 27 ? 100.f : 0.f);
        }
        dsp.prepare(48000);
        dsp.tempo(120);
        dsp.sync(0);
    }
    Panel::Binding binding(bool free = false) {
        Panel::Binding b;
        b.choosePreset = [this] { ++presets; };
        b.read = [this](int i) { return values.at(static_cast<std::size_t>(i)); };
        b.parameters = [this](Panel::Phase phase, std::span<const Panel::Change> v) {
            if (phase == Panel::Phase::Begin) {
                ++starts;
                baseline = values;
                return true;
            }
            if (phase == Panel::Phase::End) {
                ++ends;
                return true;
            }
            if (phase == Panel::Phase::Cancel) {
                ++cancels;
                values = baseline;
            }
            if (phase == Panel::Phase::Change)
                ++changes;
            for (auto p : v) {
                values.at(static_cast<std::size_t>(p.id)) = p.value;
                if (p.id >= 23 && p.id <= 30)
                    dsp.set(static_cast<Core::Param>(p.id - 1), static_cast<float>(p.value * 100));
            }
            return true;
        };
        if (free) {
            b.wave = [this](Panel::Key) { return wave; };
            b.editWave = [this](Panel::Key, Panel::Phase p, std::span<const Panel::Point> pts) {
                if (p == Panel::Phase::Begin) {
                    ++starts;
                    before = wave;
                }
                if (p == Panel::Phase::Change) {
                    ++changes;
                    wave.assign(pts.begin(), pts.end());
                }
                if (p == Panel::Phase::Cancel) {
                    ++cancels;
                    wave = before;
                }
                if (p == Panel::Phase::End)
                    ++ends;
                return true;
            };
        }
        return b;
    }
};
juce::MouseEvent event(Panel &p, float x, float y, int clicks = 1, bool alt = false, bool shift = false) {
    juce::Point<float> pos{x, y};
    auto tick = juce::Time::getCurrentTime();
    return {juce::Desktop::getInstance().getMainMouseSource(),
            pos,
            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier |
                               (alt ? juce::ModifierKeys::altModifier : 0) |
                               (shift ? juce::ModifierKeys::shiftModifier : 0)),
            0,
            0,
            0,
            0,
            0,
            &p,
            &p,
            tick,
            pos,
            tick,
            clicks,
            false};
}
void gestures() {
    Fixture f;
    Panel p;
    p.setSize(1000, 600);
    p.bind(f.binding());
    p.mouseDown(event(p, 52, 475));
    p.mouseDrag(event(p, 52, 348));
    expect(f.starts == 1 && f.changes == 1 && f.ends == 0, "change is delivered while dragging");
    float l = 1, r = 1, ol = 0, orr = 0;
    f.dsp.process(&l, &r, &ol, &orr, 1);
    expect(std::abs(ol - .5f) < .001f && ol == orr, "drag before release is audible in both channels");
    p.mouseDrag(event(p, 52, 280));
    p.mouseUp(event(p, 52, 280));
    expect(f.starts == 1 && f.changes == 2 && f.ends == 1 && f.values[23] > .7,
           "one gesture covers two pointer changes");
    const auto saved = f.values;
    p.mouseDown(event(p, 100, 300));
    p.mouseDrag(event(p, 100, 425));
    p.cancel();
    expect(f.cancels == 1 && f.values == saved, "cancelling restores prior values");
    p.mouseDown(event(p, 22, 283)); // draw tool
    p.mouseDown(event(p, 65, 440));
    p.mouseDrag(event(p, 740, 230));
    p.mouseUp(event(p, 740, 230));
    expect(f.ends == 2 && f.values[24] != saved[24] && f.values[26] != saved[26],
           "one stroke edits several real parameters");
    const auto oldCount = f.ends;
    const double old = f.values[23];
    p.mouseDown(event(p, 385, 150));
    p.mouseDrag(event(p, 385, 130));
    expect(f.values[23] > old && f.ends == oldCount, "level fader changes the active curve during the drag");
    p.mouseUp(event(p, 385, 130));
    expect(f.ends == oldCount + 1, "level fader uses one gesture for the entire contour");
    p.mouseDown(event(p, 450, 20));
    expect(f.presets == 1, "preset button reaches the host");
    p.select({4, 2, 1});
    expect(p.selection() == Panel::Key{4, 2, 1}, "per-stage secondary contour selection");
    p.select({7, 1, 0});
    expect(p.selection().stage == 6, "Time requires an advertised Time stage");
    p.bind(f.binding(true));
    p.select({0, 1, 0});
    p.mouseDown(event(p, 22, 250)); // select tool
    p.mouseDoubleClick(event(p, 280, 340, 2));
    expect(f.wave.size() == 9, "inserting a free point writes a wave");
    const auto wave = f.wave;
    auto at = p.curveBounds();
    const float px = at.getX() + .25f * at.getWidth();
    p.mouseDown(event(p, px, 340));
    p.mouseDrag(event(p, px + 40, 329));
    p.mouseUp(event(p, px + 40, 329));
    expect(f.wave.size() == wave.size(), "moving a free point keeps its neighbour count");
    p.mouseDown(event(p, 150, 300, 1, true));
    p.mouseDrag(event(p, 150, 250, 1, true));
    p.mouseUp(event(p, 150, 250));
    expect(std::any_of(f.wave.begin(), f.wave.end(),
                       [](auto a) { return a.shape == Panel::Shape::Bend && a.bend != 0; }),
           "drag bends a segment");
    auto count = f.ends;
    p.mouseDown(event(p, 90, 560));
    expect(f.ends == count + 1 && f.wave.size() == 3, "wave template is one edit");
    p.mouseDown(event(p, 22, 283)); // free pencil
    count = f.ends;
    p.mouseDown(event(p, 239, 292));
    p.mouseDrag(event(p, 371, 350));
    p.mouseDrag(event(p, 640, 270));
    p.mouseUp(event(p, 640, 270));
    expect(f.ends == count + 1 && f.wave.size() == 5,
           "free pencil preserves all pointer samples in one gesture");
    p.select({4, 1, 0});
    p.mouseDown(event(p, 750, 160));
    expect(f.values[Core::SweepMidPolarity + 1] == 1., "selected Sweep band's polarity changes");
    p.select({5, 1, 0});
    auto trim = Core::driveParam(Core::Band::Mid, Core::InputTrim) + 1;
    p.mouseDown(event(p, 675, 118));
    p.mouseDrag(event(p, 675, 85));
    p.mouseUp(event(p, 675, 85));
    expect(f.values[trim] > .29 && f.values[trim] < .31, "Drive input trim targets its own band");
    p.select({0, 1, 0});
    for (int i = 0; i < 3; ++i) {
        const int step = f.ends;
        p.mouseDown(event(p, 470.f + 70.f * i, 505));
        expect(f.ends == step + 1 && f.wave[0].shape == Panel::Shape::SourceCorner && f.wave[0].bend == i - 1,
               "one undo group sets a hard, medium or soft corner");
    }
    p.mouseDown(event(p, 22, 250));
    const auto corner = f.wave.front();
    auto plot = p.curveBounds();
    const int changes = f.ends;
    p.mouseDown(event(p, plot.getX() + corner.x * plot.getWidth(),
                      plot.getBottom() - corner.y * plot.getHeight(), 1, false, true));
    expect(f.ends == changes + 1 && f.wave.front().bend == -1,
           "shift-click cycles soft to hard in one gesture");
}
void cornerLevel() {
    Fixture f;
    f.wave = {{0, .1f, -1, Panel::Shape::SourceCorner},
              {.125f, .4f, 1, Panel::Shape::SourceCorner},
              {.25f, .9f, 0, Panel::Shape::SourceCorner},
              {.75f, .9f, 1, Panel::Shape::SourceCorner}};
    Panel p;
    p.setSize(1000, 600);
    p.bind(f.binding(true));
    p.mouseDown(event(p, 380, 150));
    p.mouseDrag(event(p, 380, 140.4f));
    p.mouseUp(event(p, 380, 140.4f));
    double sum = 0;
    for (auto point : f.wave)
        sum += point.y;
    expect(std::abs(sum / static_cast<double>(f.wave.size()) - .675) < 1.e-5 && f.ends == 1,
           "fader moves the arithmetic mean of the point heights for irregular spacing");
}
void syncDivisions() {
    Fixture f;
    Panel panel;
    panel.bind(f.binding());
    constexpr std::array<double, 23> quarters{1. / 32, 1. / 16, 1. / 8, 1. / 6, 1. / 4, 3. / 8, 1. / 3, 1. / 2,
                                              3. / 4,  2. / 3,  1,      3. / 2, 4. / 3, 2,      3,      4,
                                              6,       8,       12,     16,     32,     64,     128};
    for (std::size_t i = 0; i < quarters.size(); ++i) {
        const int before = f.ends;
        panel.selectSyncDivision(static_cast<int>(i));
        expect(std::abs(std::exp2(f.values[20] * 12 - 5) - quarters[i]) < 1e-6 && f.ends == before + 1,
               "each clock setting, including triplet and dotted values, commits one parameter edit");
    }
    const auto count = f.ends;
    f.values[19] = 1;
    panel.selectSyncDivision(9);
    expect(f.ends == count, "Hz mode keeps the continuous rate control");
}
void cachedDrawing() {
    Fixture fixture;
    Panel panel;
    panel.setSize(1000, 600);
    panel.bind(fixture.binding());
    const auto render = [&](float scale) {
        juce::Image image(juce::Image::RGB, juce::roundToInt(panel.getWidth() * scale),
                          juce::roundToInt(panel.getHeight() * scale), true);
        juce::Graphics graphics(image);
        graphics.addTransform(juce::AffineTransform::scale(scale));
        panel.paintEntireComponent(graphics, true);
        return image;
    };
    const auto before = render(1);
    std::array<float, 32> low{}, high{};
    low.fill(-.5f);
    high.fill(.5f);
    panel.setTrace({low, high}, {low, high}, .5f, .25f, .5f);
    const auto live = render(1);
    expect(live.getPixelAt(621, 339) != before.getPixelAt(621, 339) &&
               live.getPixelAt(210, 23) == before.getPixelAt(210, 23),
           "live overlay updates without changing chrome");
    for (int id = 23; id <= 30; ++id)
        fixture.values[static_cast<std::size_t>(id)] = 1.;
    panel.refresh();
    const auto updated = render(1);
    expect(updated.getPixelAt(211, 217) != live.getPixelAt(211, 217),
           "external automation invalidates cached contour");
    panel.setSize(500, 300);
    const auto dense = render(2);
    expect(dense.getPixelAt(211, 217) == updated.getPixelAt(211, 217),
           "resized high-density contour follows plot coordinates");
}
std::vector<float> samples(const char *path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("cannot open audio fixture");
    const auto bytes = static_cast<std::streamsize>(input.tellg());
    expect(bytes > 0 && bytes % sizeof(float) == 0, "invalid audio length");
    std::vector<float> values(static_cast<std::size_t>(bytes) / sizeof(float));
    input.seekg(0);
    expect(static_cast<bool>(input.read(reinterpret_cast<char *>(values.data()), bytes)), "incomplete audio");
    return values;
}
void spectrumFrames() {
    using Scope = adi::dsp::AdiShaperScope;
    adi::ui::AdiShaperSpectrum display;
    constexpr int n = Scope::fftSize, bin = 64;
    std::array<float, n> left{}, right{};
    Scope::SpectrumFrame packet;
    Scope scope;
    scope.select(0, 1);
    unsigned long long phase = 0;
    constexpr std::uint64_t step = 1ULL << 32;
    const auto feed = [&](int offset, int count, double sr) {
        scope.begin(1, left.data() + offset, right.data() + offset, static_cast<std::size_t>(count), phase,
                    step, sr);
        scope.end(left.data() + offset, right.data() + offset, static_cast<std::size_t>(count));
        phase += step * static_cast<std::uint64_t>(count);
    };
    for (double rate : {44100., 48000., 96000.}) {
        for (int i = 0; i < n; ++i) {
            const float tone = static_cast<float>(std::sin(2. * std::numbers::pi * bin * i / n));
            left[i] = .25f * tone;
            right[i] = .75f * tone;
        }
        feed(0, 17, rate);
        feed(17, 1000, rate);
        expect(!scope.readSpectrum(packet), "partial FFT packet must remain private");
        feed(1017, n - 1017, rate);
        expect(scope.readSpectrum(packet) && packet.sampleRate == rate, "complete irregular-block FFT packet");
        for (int i = 0; i < n; ++i)
            expect(packet.mono[i] == .5f * (left[i] + right[i]),
                   "FFT packet preserves consecutive stereo input");
        const auto db = display.analyse(packet);
        const auto peak = std::max_element(db.begin(), db.end());
        const double expected = bin * rate / n;
        const double actual =
            20. * std::pow(1000., static_cast<double>(peak - db.begin()) / static_cast<double>(db.size() - 1));
        expect(std::abs(*peak + 6.0206f) < .005f && std::abs(std::log(expected / actual)) < .025,
               "FFT displays calibrated tone at its log frequency");
        for (int i = 0; i < n; ++i)
            right[i] = -left[i];
        feed(0, n, rate);
        expect(scope.readSpectrum(packet), "whole-block spectrum");
        const auto silent = display.analyse(packet);
        expect(*std::max_element(silent.begin(), silent.end()) < -120.f,
               "opposite channels null in mono input spectrum");
    }
    feed(0, 137, 48000);
    phase += step * 1000;
    feed(0, n - 137, 48000);
    expect(!scope.readSpectrum(packet), "locate discards an unfinished FFT window");
    feed(0, 137, 48000);
    scope.suspend();
    scope.select(0, 1);
    expect(!scope.readSpectrum(packet), "reopened editor rejects old packets even on the same curve");
    feed(0, 137, 48000);
    feed(0, n - 137, 96000);
    expect(!scope.readSpectrum(packet), "sample rate change restarts the window");
}
void capture(const char *program, const char *destination, bool time, const char *leftFile,
             const char *rightFile, int startFrame, bool corner = false) {
    auto input = samples(program);
    constexpr int bar = 96000;
    expect(input.size() == 8u * 48000 * 2, "expected eight seconds of stereo input at 48 kHz");
    const int finalBar = static_cast<int>(input.size() / 2) - bar;
    Fixture f;
    Panel p;
    if (corner) {
        using Bank = adi::dsp::AdiShaperWaveBank;
        f.values[20] = 5. / 12;
        f.dsp.set(Core::bandParam(Core::Stage::Volume, Core::Band::Mid, Core::Cycle), 1);
        f.wave = {{0, .22f, -1, Panel::Shape::SourceCorner},
                  {1531.f / 8192, .8f, 1, Panel::Shape::SourceCorner},
                  {5099.f / 8192, .05f, 0, Panel::Shape::SourceCorner},
                  {6878.f / 8192, .65f, 1, Panel::Shape::SourceCorner}};
        int host = Bank::firstId;
        for (auto point : f.wave) {
            const Bank::Point packed{point.x, point.y, point.bend, Bank::Shape::SourceCorner};
            f.dsp.set(static_cast<Core::Param>(host++ - 1),
                      static_cast<float>(Bank::address(Bank::key(0, 1, 0), packed)));
            f.dsp.set(static_cast<Core::Param>(host++ - 1), static_cast<float>(Bank::value(packed)));
        }
    }
    auto binding = f.binding(corner);
    auto &scope = *f.dsp.shaperScope();
    const int key = (time ? 7 : 0) * 3 + 1;
    scope.select(time ? 7 : 0, 1);
    std::vector<float> nativeLeft, nativeRight;
    if (time) {
        for (int i = 23; i <= 30; ++i)
            f.values[static_cast<std::size_t>(i)] = 1;
        f.values[416] = 1;
        f.values[418] = 1. / 3;
        f.values[419] = 2. / 3;
        f.values[436] = 1. / 7;
        f.values[437] = 1;
        f.values[439] = 7. / 12;
        binding.parameterCount = 463;
        nativeLeft = samples(leftFile);
        nativeRight = samples(rightFile);
        const int offset = finalBar - startFrame;
        expect(offset >= 0 && nativeLeft.size() == nativeRight.size() &&
                   static_cast<std::size_t>(offset + bar) <= nativeLeft.size(),
               "Time output lacks the last complete bar");
    }
    std::array<float, 256> l{}, r{}, ol{}, orr{};
    const auto step = static_cast<std::uint64_t>(std::ldexp(1., 64) / bar);
    for (int i = 0; i < finalBar + bar; i += 256) {
        const int n = std::min(256, finalBar + bar - i);
        for (int j = 0; j < n; ++j) {
            l[j] = input[2u * (i + j)];
            r[j] = input[2u * (i + j) + 1];
        }
        if (time) {
            scope.begin(key, l.data(), r.data(), static_cast<std::size_t>(n),
                        step * static_cast<std::uint64_t>(i), step, 48000.);
            for (int j = 0; j < n; ++j) {
                const int offset = i + j - startFrame;
                ol[j] = offset < 0 ? 0.f : nativeLeft[static_cast<std::size_t>(offset)];
                orr[j] = offset < 0 ? 0.f : nativeRight[static_cast<std::size_t>(offset)];
            }
            scope.end(ol.data(), orr.data(), static_cast<std::size_t>(n));
        } else {
            f.dsp.sync(static_cast<double>(i) / 24000.);
            f.dsp.process(l.data(), r.data(), ol.data(), orr.data(), static_cast<std::size_t>(n));
        }
    }
    adi::dsp::AdiShaperScope::Frame waveform;
    adi::dsp::AdiShaperScope::SpectrumFrame fftInput;
    expect(scope.read(waveform) && scope.readSpectrum(fftInput), "live scope has both complete publications");
    expect(*std::max_element(waveform.outputMax.begin(), waveform.outputMax.end()) > .01f,
           "fixture produced no audio");
    adi::ui::AdiShaperSpectrum display;
    p.bind(std::move(binding));
    p.select({time ? 7 : 0, 1, 0});
    p.setSize(1300, 794);
    p.setSpectrum(display.analyse(fftInput));
    p.setTrace({waveform.inputMin, waveform.inputMax}, {waveform.outputMin, waveform.outputMax},
               waveform.peakLeft, waveform.peakRight, waveform.phase);
    juce::Image frame(juce::Image::RGB, 1920, 1080, true);
    {
        juce::Graphics g(frame);
        g.fillAll(juce::Colour(30, 32, 35));
        g.reduceClipRegion(310, 143, 1300, 794);
        g.addTransform(juce::AffineTransform::translation(310, 143));
        p.paintEntireComponent(g, true);
    }
    juce::File file =
        juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(destination));
    juce::FileOutputStream stream(file);
    expect(stream.openedOk(), "cannot create panel image");
    stream.setPosition(0);
    stream.truncate();
    expect(juce::PNGImageFormat().writeImageToStream(frame, stream), "PNG encoding failed");
}
} // namespace
int main(int argc, char **argv) try {
    juce::ScopedJuceInitialiser_GUI gui;
    gestures();
    cornerLevel();
    syncDivisions();
    cachedDrawing();
    spectrumFrames();
    if (argc == 3)
        capture(argv[1], argv[2], false, nullptr, nullptr, 0);
    else if (argc == 4 && std::string_view(argv[3]) == "corner")
        capture(argv[1], argv[2], false, nullptr, nullptr, 0, true);
    else if (argc == 7 && std::string_view(argv[3]) == "time")
        capture(argv[1], argv[2], true, argv[4], argv[5], std::stoi(argv[6]));
    else
        expect(argc == 1,
               "usage: panel-tests [stereo-input image [time output-left output-right first-frame]]");
    std::puts("ADI-Shaper panel checks passed");
    return 0;
} catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
}
