// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the exact production renderer and WAV writer, without a device.
#define main adi_play_command_main
#include "../src/juce/play.cpp"
#undef main
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>

int main() {
    adi::test::TempDirectory scratch("play_render", "channels");
    adi::StoreError error{};
    auto store=adi::Store::create(scratch.path()/"render.adi",error);
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
