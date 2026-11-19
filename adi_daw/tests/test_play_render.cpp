// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the exact production renderer and WAV writer, without a device.
#define main adi_play_command_main
#include "../src/juce/play.cpp"
#undef main
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>

int main() {
    adi::test::TempDirectory scratch("play_render", "channels");
    const auto parsed=[](std::vector<std::string> words,Options& options) {
        std::vector<char*> arguments;for(auto& word:words)arguments.push_back(word.data());
        return parse(static_cast<int>(arguments.size()),arguments.data(),options);
    };
    for(const auto workers:{0,5,11})for(const auto block:{256,2048}) {
        Options options;
        if(!parsed({"adi_play","private.adi","--workers",std::to_string(workers),"--block",std::to_string(block),
            "--diagnostics","private-run","--search-only",scratch.path().string()},options) ||
            options.workers!=workers || options.block!=block || !options.searchOnly || options.diagnosticCapacity!=262144)return 1;
    }
    for(const auto* invalid:{"-1","17","5junk"}){Options options;if(parsed({"adi_play","private.adi","--workers",invalid},options))return 1;}
    {Options options;if(parsed({"adi_play","private.adi","--diagnostics","private-run","--dry"},options))return 1;}
    {Options options;if(parsed({"adi_play","private.adi","--diagnostic-capacity","10"},options))return 1;}
    {Options options;if(!parsed({"adi_play","private.adi"},options) || options.workers!=-1 || options.searchOnly || !options.diagnostics.empty())return 1;}
    {
        adi_play::CallbackTimings timings(2);adi_play::CallbackTiming row;
        row.startTicks=100;row.endTicks=200;row.frames=256;row.sampleRate=48000;
        {adi::audio::CallbackScope callback;timings.record(row);timings.record(row);timings.record(row);}
        if(timings.observed()!=3 || timings.rows().size()!=2 || timings.dropped()!=1)return 1;
        const auto prefix=(scratch.path()/"diagnostic-test").string();std::string diagnosticError;
        if(!adi_play::writeDiagnostics(prefix,timings,{{"status","TEST: synthetic timing rows; no device opened"}},0,diagnosticError))return 1;
        std::ifstream json(adi_play::diagnosticPath(prefix,".json"));const auto receipt=nlohmann::json::parse(json);
        if(receipt.at("capture").at("dropped")!=1 || receipt.at("after_ready").at("callbacks")!=2)return 1;
        std::ifstream csv(adi_play::diagnosticPath(prefix,".callbacks.csv"));std::string line;int lines=0;
        while(std::getline(csv,line))++lines;if(lines!=3)return 1;
    }
    adi::StoreError error{};
    auto store=adi::Store::createEmpty(scratch.path()/"render.adi",error);
    if(!store)return 1;
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Tone'),(2,'master','Master')");
    for(int channels : {1,4,6}) {
        SineNode tone(220,0.125f);
        adi::engine::Session session;
        const std::uint32_t workers=channels==1?0U:channels==4?5U:11U;
        if(!session.setParallelRendering(workers,1,2))return 1;
        session.setSourcesFor([&](std::int64_t id){return id==1 ? std::vector<adi::engine::Node*>{&tone} : std::vector<adi::engine::Node*>{};});
        adi::engine::SessionSpec spec;spec.channels=channels;spec.sampleRate=48000;spec.maxFrames=64;
        if(!session.load(*store,{},spec))return 1;
        // Worker count is an upper bound: this single-track graph need not
        // contain a wide eligible level. Do not mistake that for failed setup.
        if(!session.graph().currentGraph() || session.graph().currentGraph()->renderWorkerCount()>workers)return 1;
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
    std::printf("PASS -- diagnostic parsing/capture bounds and configured 0/5/11-worker mono/four/six-channel offline Session captures; no device opened\n");
    return 0;
}
