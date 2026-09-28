// SPDX-License-Identifier: AGPL-3.0-only
#include "BandModel.hpp"
#include "Baseline.hpp"
#include "PluginEditor.hpp"
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
thread_local bool countAllocations = false;
thread_local size_t allocations = 0;
void *operator new(size_t n) {
    if (countAllocations)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }
using namespace adi::eq;
int checks = 0, failures = 0;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}
void parameter(juce::AudioProcessorValueTreeState &tree, const juce::String &id, float x) {
    auto *p = tree.getParameter(id);
    p->setValueNotifyingHost(p->convertTo0to1(x));
}
int main(int argc, char **argv) {
    juce::ScopedJuceInitialiser_GUI juce;
    double worst = 0, worstMagnitude = 0;
    check(zlp::PFilterType::kChoices.size() == 11 && zlp::POrder::kChoices.size() == 7,
          "gesture shape/slope counts match pinned source");
    check(zlp::PFreq::kRange.start == 10 && zlp::PFreq::kRange.end == 160000 &&
              zlp::PGain::kRange.start == -30 && zlp::PGain::kRange.end == 30 &&
              std::abs(zlp::PQ::kRange.start - .025) < 1e-8 && zlp::PQ::kRange.end == 25,
          "gesture limits match pinned source");
    // Test actual adapted processBlock against independent upstream processor,
    // not two copies of the graph response helper. Includes all eleven shapes.
    for (double rate : {44100., 48000., 96000.})
        for (int shape = 0; shape < 11; ++shape) {
            auto ours = std::make_unique<AdiEqProcessor>();
            auto zl = std::make_unique<ZlBaselineProcessor>();
            Band b;
            b.shape = static_cast<Shape>(shape);
            b.frequency = 1700;
            b.gain = 9;
            b.q = 1.2;
            writeBand(*ours, 0, b);
            parameter(zl->parameters_, "filter_status0", 2);
            parameter(zl->parameters_, "filter_type0", static_cast<float>(shape));
            parameter(zl->parameters_, "freq0", 1700);
            parameter(zl->parameters_, "gain0", 9);
            parameter(zl->parameters_, "q0", 1.2f);
            ours->setRateAndBufferSizeDetails(rate, 256);
            zl->setRateAndBufferSizeDetails(rate, 256);
            ours->prepareToPlay(rate, 256);
            zl->prepareToPlay(rate, 256);
            juce::AudioBuffer<float> a(4, 256), z(4, 256);
            juce::MidiBuffer midi;
            double error = 0;
            std::array<std::complex<double>, 5> ha{}, hz{};
            const std::array<double, 5> frequencies{80, 450, 1700, 5000, 14000};
            for (int block = 0; block < 160; ++block) {
                for (int i = 0; i < 256; ++i) {
                    const int n = block * 256 + i;
                    const auto input = static_cast<float>(.1 * std::sin(n * .073) + .07 * std::cos(n * .319));
                    for (int ch = 0; ch < 4; ++ch) {
                        a.setSample(ch, i, input);
                        z.setSample(ch, i, input);
                    }
                }
                ours->processBlock(a, midi);
                zl->processBlock(z, midi);
                for (int i = 0; i < 256; ++i) {
                    error =
                        std::max(error, std::abs(static_cast<double>(a.getSample(0, i) - z.getSample(0, i))));
                    if (block >= 80)
                        for (size_t f = 0; f < frequencies.size(); ++f) {
                            const auto e = std::polar(1., -2 * std::numbers::pi * frequencies[f] *
                                                              (block * 256 + i) / rate);
                            ha[f] += static_cast<double>(a.getSample(0, i)) * e;
                            hz[f] += static_cast<double>(z.getSample(0, i)) * e;
                        }
                }
            }
            worst = std::max(worst, error);
            check(error < 1e-6, "ZL time-domain parity all shapes/rates");
            for (size_t f = 0; f < ha.size(); ++f)
                worstMagnitude = std::max(worstMagnitude, std::abs(std::abs(ha[f]) - std::abs(hz[f])));
            check(worstMagnitude < 1e-6, "ZL measured magnitude parity");
            ours->releaseResources();
            zl->releaseResources();
        }
    auto p = std::make_unique<AdiEqProcessor>();
    Band b;
    b.dynamic = true;
    b.gain = 5;
    b.range = -12;
    b.frequency = 2345;
    b.q = 2;
    writeBand(*p, 0, b);
    auto read = readBand(*p, 0);
    check(std::abs(read.range + 12) < .02 && read.dynamic, "signed range maps to target gain");
    juce::MemoryBlock state;
    p->getStateInformation(state);
    auto restored = std::make_unique<AdiEqProcessor>();
    restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    read = readBand(*restored, 0);
    check(std::abs(read.frequency - 2345) < .2 && std::abs(read.range + 12) < .02 && read.dynamic,
          "state roundtrip");
    check(response(Band{}, 48000, 1000) > .999, "identity curve magnitude");
    b = Band{};
    b.gain = 12;
    check(std::abs(20 * std::log10(response(b, 48000, 1000)) - 12) < .01, "graph bell magnitude");
    // Dynamics: compare the real attack/release envelope under stepped levels,
    // for internal and external sidechain, in all three dynamic IIR structures.
    double dynamicDifference = 0;
    for (int structure = 0; structure < 3; ++structure)
        for (bool external : {false, true}) {
            auto ours = std::make_unique<AdiEqProcessor>();
            auto zl = std::make_unique<ZlBaselineProcessor>();
            Band dynamic;
            dynamic.dynamic = true;
            dynamic.range = -12;
            writeBand(*ours, 0, dynamic);
            for (auto *tree : {&ours->parameters_, &zl->parameters_}) {
                parameter(*tree, "filter_status0", 2);
                parameter(*tree, "dynamic_on0", 1);
                parameter(*tree, "target_gain0", -12);
                parameter(*tree, "threshold0", -30);
                parameter(*tree, "attack0", 10);
                parameter(*tree, "release0", 50);
                parameter(*tree, "total_external_side", external ? 1.f : 0.f);
                parameter(*tree, "total_filter_structure", static_cast<float>(structure));
            }
            ours->setRateAndBufferSizeDetails(48000, 256);
            zl->setRateAndBufferSizeDetails(48000, 256);
            ours->prepareToPlay(48000, 256);
            zl->prepareToPlay(48000, 256);
            juce::AudioBuffer<float> a(4, 256), z(4, 256);
            juce::MidiBuffer midi;
            double error = 0, wetEnergy = 0, dryEnergy = 0;
            for (int block = 0; block < 240; ++block) {
                for (int i = 0; i < 256; ++i)
                    for (int ch = 0; ch < 4; ++ch) {
                        const auto level = (block < 80 || block > 180) ? .001 : .2;
                        const float x =
                            static_cast<float>((ch < 2 ? .1 : level) * std::sin(2 * std::numbers::pi * 1000 *
                                                                                (block * 256 + i) / 48000));
                        a.setSample(ch, i, x);
                        z.setSample(ch, i, x);
                        if (ch == 0 && block > 100 && block < 170)
                            dryEnergy += x * x;
                    }
                ours->processBlock(a, midi);
                zl->processBlock(z, midi);
                for (int i = 0; i < 256; ++i) {
                    error =
                        std::max(error, std::abs(static_cast<double>(a.getSample(0, i) - z.getSample(0, i))));
                    if (block > 100 && block < 170)
                        wetEnergy += a.getSample(0, i) * a.getSample(0, i);
                }
            }
            dynamicDifference = std::max(dynamicDifference, error);
            check(error < 1e-6, "dynamic envelope and sidechain upstream parity");
            check(wetEnergy < dryEnergy * .9, "dynamic range audibly attenuates above threshold");
        }
    // Verify GUI opens/renders without running a device or playing sound.
    writeBand(*p, 0, b);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());
    check(editor != nullptr, "graph editor exists");
    {
        auto fresh = std::make_unique<AdiEqProcessor>();
        AdiEqEditor graph(*fresh);
        auto event = [&](juce::Point<float> pos, juce::Point<float> down, int flags, bool dragged) {
            return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos,
                                    juce::ModifierKeys(flags), 1, 0, 0, 0, 0, &graph, &graph,
                                    juce::Time::getCurrentTime(), down, juce::Time::getCurrentTime(), 1,
                                    dragged);
        };
        const juce::Point<float> down{400, 280}, up{400, 240};
        const int alt = juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier;
        graph.mouseDown(event(down, down, alt, false));
        graph.mouseDrag(event(up, down, alt, true));
        auto made = readBand(*fresh, 0);
        check(value(*fresh, zlp::PFilterStatus::kID, 0) == 2 && made.dynamic && made.gain > 0 &&
                  made.range < 0,
              "GUI Alt curve drag creates active dynamic band");
        juce::MouseWheelDetails wheel{};
        wheel.deltaY = .1f;
        graph.mouseWheelMove(event(up, down, juce::ModifierKeys::leftButtonModifier, true), wheel);
        auto wheeled = readBand(*fresh, 0);
        check(wheeled.q > made.q && std::abs(wheeled.gain - made.gain) < .02,
              "GUI wheel while dragging edits current Q");
        graph.mouseUp(event(up, down, alt, true));
        graph.mouseDown(event(up, up, alt, false));
        graph.mouseUp(event(up, up, alt, false));
        check(readBand(*fresh, 0).bypass, "GUI Alt click bypass");
        graph.mouseDown(event(up, up, juce::ModifierKeys::leftButtonModifier, false));
        graph.mouseDrag(event({up.x, up.y - 10}, up,
                              juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::ctrlModifier,
                              true));
        graph.mouseUp(event({up.x, up.y - 10}, up, juce::ModifierKeys::ctrlModifier, true));
        check(readBand(*fresh, 0).q > wheeled.q, "GUI Ctrl vertical drag edits Q");
    }
    p->setRateAndBufferSizeDetails(48000, 256);
    p->prepareToPlay(48000, 256);
    juce::AudioBuffer<float> allocationBuffer(4, 256);
    allocationBuffer.clear();
    juce::MidiBuffer emptyMidi;
    for (int i = 0; i < 16; ++i)
        p->processBlock(allocationBuffer, emptyMidi);
    allocations = 0;
    countAllocations = true;
    for (int i = 0; i < 32; ++i)
        p->processBlock(allocationBuffer, emptyMidi);
    countAllocations = false;
    check(allocations == 0, "zero process operator-new allocations in minimum mode");
    if (argc > 1) {
        const auto picture = editor->createComponentSnapshot(editor->getLocalBounds());
        juce::FileOutputStream output(juce::File(juce::String::fromUTF8(argv[1])));
        juce::PNGImageFormat format;
        check(format.writeImageToStream(picture, output), "editor snapshot file");
    }
    std::printf("Dynamic maximum sample error %.9g\n", dynamicDifference);
    std::printf("%d checks, %d failures; worst sample error %.9g; magnitude error %.9g\n", checks, failures,
                worst, worstMagnitude);
    return failures ? 1 : 0;
}
