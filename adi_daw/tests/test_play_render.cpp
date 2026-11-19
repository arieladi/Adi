// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the exact production renderer and WAV writer, without a device.
#define main adi_play_command_main
#include "../src/juce/play.cpp"
#undef main
#include "adi/media/media_ops.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>

#if defined(ADI_CATALOGUE_FIXTURE)
namespace {
bool catalogueContracts() {
    namespace fs = std::filesystem;
    juce::ScopedJuceInitialiser_GUI initialize;
    adi::test::TempDirectory scratch("play_render", "catalogue");
    const auto builtHelper = adi::media::pathFromUtf8(ADI_CATALOGUE_SCANNER);
    adi::scan::Config config;
    config.executable = scratch.path() / builtHelper.filename();
    config.registry = scratch.path() / "plugins.sqlite";
    fs::copy_file(builtHelper, config.executable);

    adi::device::JuceDeviceLoader loader;
    loader.setScanConfig(config);
    juce::KnownPluginList fixtureTypes;
    const auto discovery = loader.vst3().scanFile(ADI_CATALOGUE_FIXTURE, fixtureTypes, config);
    const auto descriptions = fixtureTypes.getTypes();
    if (!discovery.complete || !discovery.failures.empty() || descriptions.isEmpty()) return false;

    const auto& description = descriptions.getReference(0);
    adi::rows::PluginRef ref;
    ref.format = "vst3";
    ref.uid = adi::device::identityOf(description).uid;
    ref.pathHint = description.fileOrIdentifier.toStdString();
    adi::engine::DeviceRequest request;
    request.ref = &ref;
    const auto make = loader.fn();
    std::string error;
    auto instance = make(request, error);
    if (!instance || !instance->loaded()) return false;
    instance.reset();

    // Even a persistent cache hit would try to open the configured helper.
    // A known identity or path must answer from the populated catalogue.
    if (!fs::remove(config.executable)) return false;
    instance = make(request, error);
    if (!instance || !instance->loaded()) return false;
    instance.reset();
    ref.pathHint.clear();
    instance = make(request, error);
    if (!instance || !instance->loaded()) return false;
    instance.reset();

    ref.pathHint = description.fileOrIdentifier.toStdString();
    ref.uid = "unlisted-class";
    return !make(request, error) && loader.stats().vst3 == 3 &&
           loader.stats().notFound == 1 && loader.stats().scans == 0 &&
           loader.stats().failed == 0;
}
} // namespace
#endif

int main() {
#if defined(ADI_CATALOGUE_FIXTURE)
    if (!catalogueContracts()) {
        std::printf("FAIL -- hosted VST3 catalogue lookup\n");
        return 1;
    }
    std::printf("PASS -- known VST3 identities and paths require no discovery during instance load\n");
#endif
    adi::test::TempDirectory scratch("play_render", "channels");
    adi::StoreError error{};
    auto store=adi::Store::createEmpty(scratch.path()/"render.adi",error);
    if(!store)return 1;
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Tone'),(2,'master','Master')");
    for(int channels : {1,4,6}) {
        SineNode tone(220,0.125f);
        adi::engine::Session session;
        session.setSourcesFor([&](std::int64_t id){return id==1 ? std::vector<adi::engine::Node*>{&tone} : std::vector<adi::engine::Node*>{};});
        adi::engine::SessionSpec spec;spec.channels=channels;spec.sampleRate=48000;spec.maxFrames=64;
        if(!session.load(*store,{},spec))return 1;
        session.transport().play(true);
        // Intentionally start with the old two-channel allocation: production
        // must resize it to the Session format before copying any samples.
        juce::AudioBuffer<float> capture(2,1);
        if(renderOffline(session,spec.channels,spec.sampleRate,spec.maxFrames,0.02,false,0,&capture)!=0)return 1;
        if(capture.getNumChannels()!=channels || capture.getNumSamples()!=960)return 1;
        for(int c=0;c<channels;++c)
            if(capture.getMagnitude(c,0,960)<0.01f)return 1;
        const auto path=(scratch.path()/(std::to_string(channels)+".wav")).string();
        if(!writeRender(path,spec.sampleRate,capture))return 1;
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(new juce::FileInputStream(juce::File(path)),true));
        if(!reader || reader->numChannels!=static_cast<unsigned>(channels) || reader->lengthInSamples!=960 || !reader->usesFloatingPointData)return 1;
        juce::AudioBuffer<float> decoded(channels,960);
        if(!reader->read(&decoded,0,960,0,true,true))return 1;
        for(int c=0;c<channels;++c)for(int i=0;i<960;++i)
            if(decoded.getSample(c,i)!=capture.getSample(c,i))return 1;
    }
    std::printf("PASS -- mono, four-channel and six-channel Session captures round-trip exactly\n");
    return 0;
}
