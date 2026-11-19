// SPDX-License-Identifier: GPL-3.0-or-later
// Owned SDK fixture only. No editor, desktop input or audio device.
#include "adi/ui/project_document.hpp"
#include "juce/vst3_host.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace adi;
namespace {
int checks = 0, failures = 0;
void check(bool yes, const char *label) {
    ++checks;
    if (!yes) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}
std::filesystem::path native(const std::string &s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
struct KeyProbe {
    double (*first)();
    double (*last)();
    int (*channels)();
    int (*inPlace)();

    bool silent() const { return channels() == 0 && first() == 0. && last() == 0.; }
    bool stereoKey() const { return channels() == 2 && first() == .5 && last() == .75; }
};

class StereoConstant final : public engine::Node {
  public:
    StereoConstant(float left, float right) : levels_{left, right} {}
    void process(const engine::NodeIo &io) noexcept override {
        for (std::int32_t ch = 0; ch < io.channels; ++ch)
            std::fill_n(io.out[ch] + io.blockOffset, io.frames, levels_[static_cast<std::size_t>(ch)]);
    }
  private:
    std::array<float, 2> levels_;
};

void documentContract(device::Vst3Host &host, const juce::PluginDescription &description,
                      const KeyProbe &keyProbe) {
    constexpr std::int64_t keyTrack = 2, programmeTrack = 3, recipientId = 30;
    constexpr std::int32_t frames = 256;
    const auto identity = device::identityOf(description);
    test::TempDirectory temp("vst3_key", "document_dormant_aux");
    const auto file = temp.path() / "dormant-aux.adi";
    {
        StoreError status{};
        auto store = Store::create(file, status);
        if (!store) throw std::runtime_error(toString(status));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES "
                         "(1,'master','Main'),(2,'audio','Key'),(3,'audio','Programme');"
                         "INSERT INTO device_chains(id,track_id,name) VALUES (3,3,'Devices');");
        SQLite::Statement ref(store->db(),
            "INSERT INTO plugin_refs(id,format,uid,name) VALUES (77,'vst3',?,?)");
        ref.bind(1, identity.uid);
        ref.bind(2, identity.name);
        ref.exec();
        store->db().exec("INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) "
                         "VALUES (30,3,0,77,'Dormant auxiliary');");
    }

    // The real host adapter receives the discovered owned module via the
    // document's usual loader, without scanning other plug-ins.
    const engine::DeviceLoader loader = [&](const engine::DeviceRequest &request, std::string &error)
        -> std::unique_ptr<device::DeviceInstance> {
        if (!request.ref || request.ref->format != identity.format || request.ref->uid != identity.uid) {
            error = "Unexpected device in the dormant auxiliary fixture";
            return {};
        }
        return host.makeDevice(description, request.sampleRate, request.maxFrames, error);
    };
    StereoConstant key(.5f, .75f), programme(.125f, .25f);
    const auto prepare = [&](ui::ProjectDocument &document) {
        auto &session = document.session();
        session.setSourcesFor([&](std::int64_t track) {
            if (track == keyTrack) return std::vector<engine::Node *>{&key};
            if (track == programmeTrack) return std::vector<engine::Node *>{&programme};
            return std::vector<engine::Node *>{};
        });
        session.graph().setFadeFrames(0);
        document.prepare(48000., frames);
        return document.mailbox().post(ui::TransportMailbox::Command::Play);
    };
    using Audio = std::array<std::array<float, frames>, 2>;
    const auto render = [](ui::ProjectDocument &document) {
        Audio output{};
        std::array<float *, 2> planes{output[0].data(), output[1].data()};
        engine::AudioIo io;
        io.out = planes.data();
        io.numOut = 2;
        io.frames = frames;
        document.process(io);
        return output;
    };
    const auto routed = [&](ui::ProjectDocument &document, const char *tap) {
        const auto &routes = document.session().model().routing;
        return routes.size() == 1 && routes.front().srcId == keyTrack &&
               routes.front().dstId == recipientId && routes.front().keyTap == tap;
    };
    const auto presented = [](ui::ProjectDocument &document) {
        const auto publication = document.parameterFeed().current();
        const auto found = std::find_if(publication->begin(), publication->end(),
            [](const ui::DevicePanelData &panel) { return panel.id == recipientId; });
        if (found == publication->end()) throw std::runtime_error("The fixture device panel was not published");
        return *found;
    };

    std::string error;
    auto document = ui::ProjectDocument::open(file, false, loader, error);
    if (!document) throw std::runtime_error(error);
    check(prepare(*document), "the document audio callback accepts a queued Play");
    document->tick(1);
    auto *device = dynamic_cast<device::Vst3Device *>(document->session().instanceFor(recipientId));
    if (!device) throw std::runtime_error("The document did not load the owned VST3 adapter");
    check(device->usingRawProcessor() && device->sidechainInputChannels() == 2,
          "the real graph has a declared but initially dormant stereo auxiliary bus");
    const auto baseline = render(*document);
    check(keyProbe.silent() && keyProbe.inPlace() == 1 &&
              std::all_of(baseline[0].begin(), baseline[0].end(), [](float sample) { return sample > 0.f; }) &&
              std::all_of(baseline[1].begin(), baseline[1].end(), [](float sample) { return sample > 0.f; }),
          "programme plays through the actual SDK callback with the dormant key closed");

    check(document->setDeviceKey(presented(*document), keyTrack, "post_fx") && routed(*document, "post_fx"),
          "assigning the key through ProjectDocument commits the intended post-FX route");
    (void)render(*document);
    check(keyProbe.stereoKey(), "the first callback after assigning a dormant key receives both auxiliary channels");
    const auto configuredActivations = device->activations();
    const auto *timelineReader = document->session().clips();

    check(document->setDeviceKey(presented(*document), keyTrack, "pre_fx") && routed(*document, "pre_fx"),
          "a retap changes the stored source tap without changing bus width");
    const auto retapped = render(*document);
    check(keyProbe.stereoKey() && retapped == baseline && device->activations() == configuredActivations &&
              document->session().clips() == timelineReader,
          "a retap with the same auxiliary shape preserves the SDK, timeline reader and programme");

    check(document->setDeviceKey(presented(*document), {}, "pre_fx") && document->session().model().routing.empty(),
          "removing the route from the document clears just its key");
    (void)render(*document);
    check(keyProbe.silent(), "the first callback after removal closes the default-inactive auxiliary again");
    check(document->ops().undo().ok && document->synchronise() && routed(*document, "pre_fx"),
          "one Undo restores the complete pre-FX key route");
    (void)render(*document);
    check(keyProbe.stereoKey(), "Undo reopens the dormant bus and restores the key samples");
    check(document->ops().redo().ok && document->synchronise() && document->session().model().routing.empty(),
          "one Redo removes the route again");
    (void)render(*document);
    check(keyProbe.silent(), "Redo closes the auxiliary bus again");

    check(document->ops().undo().ok && document->synchronise() && routed(*document, "pre_fx"),
          "leave the authored route for a clean reopen");
    document->release();
    document.reset();
    auto reopened = ui::ProjectDocument::open(file, false, loader, error);
    if (!reopened) throw std::runtime_error(error);
    check(routed(*reopened, "pre_fx") && prepare(*reopened), "the reopened project reads the stored route");
    (void)render(*reopened);
    check(keyProbe.stereoKey(), "first reopened audio callback hears the stored key without a new edit");
    reopened->release();
}

void contract() {
    test::TempDirectory temp("vst3_key", "owned");
    scan::Config config;
    config.executable = native(ADI_SCAN_HELPER);
    config.registry = temp.path() / "registry.sqlite";
    device::Vst3Host host;
    juce::KnownPluginList list;
    const auto scan = host.scanFile(juce::String::fromUTF8(ADI_TEST_VST3), list, config);
    check(scan.complete && list.getNumTypes() == 3,
          "dedicated scanner discovers only owned fixture variants");
    // Observe the exact module the format loaded inside its bundle, not the
    // separate build output (which would have independent process globals).
    const juce::File bundle(juce::String::fromUTF8(ADI_TEST_VST3));
    juce::File binary;
    for (const auto &file :
         bundle.getChildFile("Contents").findChildFiles(juce::File::findFiles, true))
        if (file.getFileNameWithoutExtension() == bundle.getFileNameWithoutExtension())
            binary = file;
    juce::DynamicLibrary module(binary.getFullPathName());
    auto first = reinterpret_cast<double (*)()>(module.getFunction("adiFixtureKeyFirst"));
    auto last = reinterpret_cast<double (*)()>(module.getFunction("adiFixtureKeyLast"));
    auto channels = reinterpret_cast<int (*)()>(module.getFunction("adiFixtureKeyChannels"));
    auto inPlace=reinterpret_cast<int(*)()>(module.getFunction("adiFixtureMainInPlace"));
    if (!first || !last || !channels || !inPlace)
        throw std::runtime_error("owned fixture observations unavailable");
    for (const auto &desc : list.getTypes())
        for (bool wide : {false, true}) {
            std::string error;
            auto instance = host.instantiate(desc, 48000, 32, error);
            if (!instance)
                throw std::runtime_error(error);
            device::Vst3Device d(std::move(instance), device::identityOf(desc));
            const bool mono = desc.name.containsIgnoreCase("Plain");
            const bool defaultActive = mono;
            const int keyWidth = mono ? 1 : 2;
            check(d.sidechainInputChannels() == keyWidth,
                  "declared auxiliary width is cached before prepare");
            d.willBeHanded(wide ? engine::Precision::Float64 : engine::Precision::Float32);
            d.prepare(48000, 32);
            const auto initialShape = d.preparationShapeEpoch();
            const auto initialActivations = d.activations();
            check(d.configureSidechain(true), "owner can request the declared key input");
            check((d.preparationShapeEpoch() == initialShape) == defaultActive,
                  defaultActive ? "connecting a default-active key preserves the preparation stamp"
                                : "connecting a dormant key changes the preparation stamp");
            d.prepare(48000, 32);
            check(d.activations() == initialActivations + (defaultActive ? 0 : 1),
                  defaultActive ? "connecting a default-active key preserves activation"
                                : "connecting a dormant key prepares the changed layout once");
            check(d.usingRawProcessor(), "test exercises production raw VST3 bus tables");
            std::array<float, 32> in{}, inR{}, keyL{}, keyR{}, outL{}, outR{};
            std::array<double, 32> in64{}, inR64{}, keyL64{}, keyR64{}, outL64{}, outR64{};
            for (std::size_t i = 0; i < 32; ++i) {
                in[i] = (static_cast<float>(i) - 16.f) / 32.f;
                inR[i] = .75f - static_cast<float>(i) / 64.f;
                // Distinct channel patterns and double-only low bits catch
                // channel swaps, offsets and an unintended float round-trip.
                in64[i] = static_cast<double>(in[i]) + static_cast<double>(i + 1) * 1e-11;
                inR64[i] = static_cast<double>(inR[i]) - static_cast<double>(i + 1) * 3e-11;
                keyL[i] = static_cast<float>(i) / 32;
                keyL64[i] = static_cast<double>(i) / 32;
                keyR[i] = .5f;
                keyR64[i] = .5;
                outL[i] = outR[i] = -1;
                outL64[i] = outR64[i] = -1;
            }
            const float *input[]{in.data(), inR.data()};
            const float *key[]{keyL.data(), keyR.data()};
            float *output[]{outL.data(), outR.data()};
            const double *input64[]{in64.data(), inR64.data()};
            const double *key64[]{keyL64.data(), keyR64.data()};
            double *output64[]{outL64.data(), outR64.data()};
            engine::NodeIo io;
            io.channels = 2;
            io.frames = 11;
            io.blockOffset = 7;
            io.sampleRate = 48000;
            if (wide) {
                io.in64 = input64;
                io.out64 = output64;
                io.sidechain64 = key64;
            } else {
                io.in = input;
                io.out = output;
                io.sidechain = key;
            }
            io.sidechainSilent = false;
            d.process(io);
            check(channels() == keyWidth && first() == 7. / 32 &&
                      last() == (mono ? 17. / 32 : .5),
                  "exact mono/stereo key segment reaches the aux bus at native/narrowed precision");
            check(inPlace()==1,"aux delivery retains the main in-place SDK fast path");
            const auto programmeExact = [&] {
                for (std::size_t i = 0; i < 32; ++i) {
                    const bool inside = i >= 7 && i < 18;
                    if (wide) {
                        const double left =
                            inside ? (d.processingDouble()
                                          ? in64[i]
                                          : static_cast<double>(static_cast<float>(in64[i])))
                                   : -1.;
                        const double right =
                            inside ? (d.processingDouble()
                                          ? inR64[i]
                                          : static_cast<double>(static_cast<float>(inR64[i])))
                                   : -1.;
                        if (outL64[i] != left || outR64[i] != right)
                            return false;
                    } else if (outL[i] != (inside ? in[i] : -1.f) ||
                               outR[i] != (inside ? inR[i] : -1.f))
                        return false;
                }
                return true;
            };
            check(programmeExact(), "all programme samples/channels retain exact supported "
                                    "precision and segment bounds");
            const auto activations = d.activations();
            d.prepare(48000, 32);
            check(d.activations() == activations, "unchanged rebuild never restarts the plugin");
            io.sidechain = nullptr;
            io.sidechain64 = nullptr;
            io.sidechainSilent = true;
            d.process(io);
            check(programmeExact(),
                  "clearing the key preserves the entire programme array exactly");
            check(first() == 0 && last() == 0,
                  "disconnected key is silent, not stale data or programme");
            const auto connectedShape = d.preparationShapeEpoch();
            check(d.configureSidechain(false), "owner can disable a formerly requested key");
            check((d.preparationShapeEpoch() == connectedShape) == defaultActive,
                  defaultActive ? "disconnecting a default-active key preserves the preparation stamp"
                                : "disconnecting a requested dormant key changes the preparation stamp");
            d.prepare(48000, 32);
            check(d.activations() == activations + (defaultActive ? 0 : 1),
                  defaultActive ? "disconnecting a default-active key preserves activation"
                                : "disconnecting a requested dormant key prepares the changed layout once");
            d.process(io);
            check(inPlace()==1 && programmeExact(),"removing the key retains in-place processing and exact full programme arrays");
            check(channels() == (defaultActive ? keyWidth : 0) && first() == 0 && last() == 0,
                  defaultActive ? "default-active aux carries silence after route removal"
                                : "default-inactive aux is deactivated after route removal");
        }
    const auto descriptions = list.getTypes();
    const auto stereo = std::find_if(descriptions.begin(), descriptions.end(), [](const auto &description) {
        return description.name == "ADI Test NoteExpr";
    });
    if (stereo == descriptions.end()) throw std::runtime_error("Owned stereo auxiliary fixture unavailable");
    documentContract(host, *stereo, {first, last, channels, inPlace});
}
} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    try {
        contract();
    } catch (const std::exception &e) {
        ++failures;
        std::printf("EXCEPTION: %s\n", e.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
