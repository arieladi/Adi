// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/project_document.hpp"
#include "juce/builtin_host.hpp"
#include "juce/mix_function_host.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <type_traits>

using namespace adi;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", label); }
}
std::unique_ptr<device::DeviceInstance> make(int channels = 2) {
    rows::PluginRef ref;
    ref.format = "internal"; ref.uid = "org.adidaw.mix.eq"; ref.name = "EQ";
    engine::DeviceRequest request; request.ref = &ref; request.channels = channels;
    std::string error;
    return device::withBuiltinDevices()(request, error);
}
template<class Sample> void splitAudio(double rate, int channels) {
    auto whole = make(channels), split = make(channels);
    check(whole && split, "ordinary native loader creates actual EQ provider");
    if (!whole || !split) return;
    check(whole->processesDouble(), "canonical EQ retains full double graph admission");
    // A nonzero full-band gain is independent of frequency/Q and detector
    // implementations, and exercises an actual processing path rather than dry pass-through.
    check(whole->setParam("9", device::ParamValue::fromNormalized(1)) &&
          whole->setParam("10", device::ParamValue::fromNormalized(1)) &&
          whole->setParam("14", device::ParamValue::fromNormalized(.6)), "flat gain band configured");
    const auto state = whole->saveState("state");
    check(!state.empty() && split->loadState("state", state), "equivalent fresh native state restores");
    whole->prepare(rate, 64); split->prepare(rate, 64);
    std::array<std::array<Sample, 64>, 2> input{}, a{}, b{};
    std::array<const Sample*, 2> in{input[0].data(), input[1].data()};
    std::array<Sample*, 2> ap{a[0].data(), a[1].data()}, bp{b[0].data(), b[1].data()};
    bool same = true;
    for (std::uint64_t block = 1; block <= 32; ++block) {
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t i = 0; i < 64; ++i)
                input[ch][i] = static_cast<Sample>(.1 * std::sin(.17 * static_cast<double>(i + block * 64 + ch * 13)));
        engine::NodeIo io; io.channels = channels; io.frames = 64;
        io.sampleRate = rate; io.renderBlock = block; io.configuredLatency = whole->latencySamples();
        if constexpr (std::is_same_v<Sample, double>) { io.in64 = in.data(); io.out64 = ap.data(); }
        else { io.in = in.data(); io.out = ap.data(); }
        whole->process(io);
        if constexpr (std::is_same_v<Sample, double>) io.out64 = bp.data(); else io.out = bp.data();
        io.frames = 17; split->process(io);
        io.blockOffset = 17; io.frames = 47; split->process(io);
        for (int ch = 0; ch < channels; ++ch) same = same && a[static_cast<std::size_t>(ch)] == b[static_cast<std::size_t>(ch)];
    }
    check(same, "native audio is exact through physical 17/47 segmentation");
    double peakError = 0;
    const double gain = std::pow(10.0, whole->getParam("14").real / 20.0);
    for (int ch = 0; ch < channels; ++ch)
        for (std::size_t i = 0; i < 64; ++i)
            peakError = std::max(peakError, std::abs(static_cast<double>(a[static_cast<std::size_t>(ch)][i]) -
                       gain * static_cast<double>(input[static_cast<std::size_t>(ch)][i])));
    check(peakError < 2e-7, "actual canonical band gain reaches audio rather than a dry stand-in");
    whole->release(); split->release();
}
void documentAuthority() {
    test::TempDirectory temp("mix_eq_pipeline", "document");
    std::string error;
    auto doc = ui::ProjectDocument::open(temp.path() / "song.adi", true, device::withBuiltinDevices(), error);
    check(doc != nullptr, "EQ integration project opens"); if (!doc) return;
    check(doc->addMixFunction(mix::Function::Eq, 2), "actual native EQ is opt-in through prepared insertion");
    doc->tick(0);
    const auto pub = doc->parameterFeed().current();
    const auto at = std::find_if(pub->begin(), pub->end(), [](const auto& d) { return d.uid == "org.adidaw.mix.eq"; });
    check(at != pub->end() && at->available, "real EQ runtime is published to common feed");
    if (at == pub->end()) return;
    doc->setParameterDemand(doc.get(), {at->id});
    const auto requested = doc->parameterFeed().current();
    const auto shown = *std::find_if(requested->begin(), requested->end(), [&](const auto& data) { return data.id == at->id; });
    check(shown.metadata && shown.parameters().size() == 730 && shown.resolved.entries.empty(),
          "actual native EQ demand shares all controls without an aggregate Configure list");
    auto* instance = doc->session().instanceFor(shown.id);
    check(instance && instance->paramCount() == 730, "complete authored processing parameter set is available");
    if (!instance) return;
    using K = engine::ParamEventKind;
    const auto original = instance->getParam("14").normalized;
    check(doc->parameterGestureExpected(shown.id, "14", K::Begin, original, shown.runtime, shown.instance) &&
          doc->parameterGestureExpected(shown.id, "14", K::Value, .6, shown.runtime, shown.instance) &&
          doc->parameterGestureExpected(shown.id, "14", K::End, .6, shown.runtime, shown.instance),
          "EQ edit follows exact-runtime Begin Value End common authority");
    check(std::abs(instance->getParam("14").real - 6) < 1e-5, "common edit reaches canonical EQ control");
    const auto undo = doc->ops().undo(); const bool undoSync = doc->synchronise(); doc->tick(1);
    check(undo.ok && undoSync && std::abs(doc->session().instanceFor(shown.id)->getParam("14").real) < 1e-5,
          "one common undo restores EQ gesture origin");
    const auto redo = doc->ops().redo(); const bool redoSync = doc->synchronise(); doc->tick(2);
    check(redo.ok && redoSync && std::abs(doc->session().instanceFor(shown.id)->getParam("14").real - 6) < 1e-5,
          "common redo restores EQ gesture value");
    check(doc->parameterGestureExpected(shown.id, "587", K::Begin, 0, shown.runtime, shown.instance) &&
          doc->parameterGestureExpected(shown.id, "587", K::Value, .75, shown.runtime, shown.instance) &&
          doc->parameterGestureExpected(shown.id, "587", K::End, .75, shown.runtime, shown.instance),
          "three-digit Band 24 placement has the same common gesture authority");
    instance = doc->session().instanceFor(shown.id);
    check(instance && instance->getParam("587").real == 3 && instance->getParam("12").real == 0,
          "Band 24 Mid edit leaves Band 1 Stereo unchanged");
    const auto placementUndo = doc->ops().undo();
    const bool placementSync = doc->synchronise(); doc->tick(3);
    check(placementUndo.ok && placementSync && doc->session().instanceFor(shown.id)->getParam("587").real == 0 &&
          std::abs(doc->session().instanceFor(shown.id)->getParam("14").real - 6) < 1e-5,
          "placement undo is one transaction and preserves the previous band gain edit");
    check(!doc->addMixFunction(mix::Function::Eq, 2), "second native EQ is refused in same channel");
    check(!doc->addMixFunction(mix::Function::Compressor, 2), "unlinked functions remain honestly unavailable");
}
void pairedBandGesture() {
    test::TempDirectory temp("mix_eq_pipeline", "paired-band");
    std::string error;
    auto doc = ui::ProjectDocument::open(temp.path() / "song.adi", true, device::withBuiltinDevices(), error);
    check(doc && doc->addMixFunction(mix::Function::Eq, 2), "paired gesture opens a native EQ document");
    if (!doc) return;
    doc->tick(0);
    const auto publication = doc->parameterFeed().current();
    const auto found = std::find_if(publication->begin(), publication->end(), [](const auto& d) {
        return d.uid == "org.adidaw.mix.eq" && d.available;
    });
    if (found == publication->end()) { check(false, "paired gesture has a runtime identity"); return; }
    const auto shown = *found;
    const auto value = [&](const char* id) { return doc->session().instanceFor(shown.id)->getParam(id).normalized; };
    const auto operations = [&] { return doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt(); };
    const auto transactions = [&] { return doc->store().db().execAndGet("SELECT count(DISTINCT txn_id) FROM ops").getInt(); };
    using E = engine::ParameterPairEvent;
    using K = engine::ParamEventKind;
    const engine::ParameterPair origin{{{"13", value("13")}, {"14", value("14")}}};
    const engine::ParameterPair final{{{"13", .62}, {"14", .67}}};
    std::shared_ptr<const gesture::OwnerToken> owner;
    auto edit = [&](E kind, const engine::ParameterPair& positions) {
        return doc->parameterPairGestureExpected(shown.id, positions, kind, shown.runtime, shown.instance, owner);
    };
    auto matches = [&](const engine::ParameterPair& positions) {
        return std::abs(value("13") - positions[0].second) < 1e-6 &&
               std::abs(value("14") - positions[1].second) < 1e-6;
    };
    const auto before = operations(), initialTransactions = transactions();
    check(edit(E::Begin, origin) && owner && edit(E::Value, final) && matches(final) && operations() == before,
          "paired live edit previews both values without journaling");
    check(!doc->parameterGestureExpected(shown.id, "14", K::Begin, .67, shown.runtime, shown.instance),
          "a scalar edit cannot grab the gain of a held paired edit");
    check(edit(E::End, final) && !owner && matches(final) &&
          operations() == before + 4 && transactions() == initialTransactions + 1 && doc->pendingParameterGestures().empty(),
          "paired End writes both first-touch openers and final values in one transaction");
    check(doc->ops().undo().ok && doc->synchronise() && matches(origin),
          "one Undo restores both original coordinates");
    check(doc->ops().redo().ok && doc->synchronise() && matches(final),
          "one Redo restores both edited coordinates");
    const auto settled = operations();
    check(edit(E::Begin, final) && edit(E::Value, origin) && matches(origin) &&
          edit(E::Cancel, origin) && !owner && matches(final) && operations() == settled,
          "cancel restores the pair and leaves no history row");
    check(edit(E::Begin, final) && edit(E::Value, origin) && matches(origin),
          "paired capture can be abandoned with both preview coordinates audible");
    owner.reset();
    doc->tick(1);
    check(matches(final) && operations() == settled && doc->pendingParameterGestures().empty(),
          "the owner pump restores a pair if its surface disappears before Cancel");
    check(edit(E::Begin, final) && edit(E::End, final) && !owner && operations() == settled,
          "a click without a drag adds no undo entry");
    auto invalid = origin;
    invalid[1].second = std::numeric_limits<double>::quiet_NaN();
    check(edit(E::Begin, final) && !edit(E::Value, invalid) && !owner && matches(final) &&
          operations() == settled && doc->pendingParameterGestures().empty(),
          "a rejected second coordinate writes neither value and cancels the pair");
    // The preview applies rows inside a rollback scope. Refuse the second
    // durable journal insertion so this checks rollback after the first one.
    doc->store().db().exec("CREATE TRIGGER refuse_pair_gain BEFORE INSERT ON ops "
                           "WHEN NEW.op_type='device.setParam' AND (SELECT count(*) FROM ops) > " +
                           std::to_string(settled) + " BEGIN SELECT RAISE(ABORT,'refuse gain'); END");
    check(edit(E::Begin, final) && edit(E::Value, origin) && matches(origin) &&
          !edit(E::End, origin) && !owner && matches(final) && operations() == settled &&
          doc->pendingParameterGestures().empty(),
          "a journal refusal on gain rolls back frequency and restores both audible values");
    doc->store().db().exec("DROP TRIGGER refuse_pair_gain");
}
} // namespace
int main() {
    for (double rate : {44100., 48000., 96000.})
        for (int channels : {1, 2}) { splitAudio<float>(rate, channels); splitAudio<double>(rate, channels); }
    documentAuthority();
    pairedBandGesture();
    std::printf("mix EQ pipeline: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
