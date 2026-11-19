// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "temp_directory.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/param_ops.hpp"
#include "adi/engine/session.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "juce/juce_device_loader.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>

namespace adi::test {
namespace scanner_reopen_detail {
using Json=nlohmann::json;namespace fs=std::filesystem;
inline std::string text(const fs::path& path){const auto bytes=path.u8string();return {bytes.begin(),bytes.end()};}
inline void require(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
inline std::int64_t nanoseconds(std::int64_t samples){return static_cast<std::int64_t>(std::llround(static_cast<double>(samples)*1e9/48000));}
inline float sample(std::int64_t frame,int channel){return channel==0?static_cast<float>(frame%127+1)/256.0f:-static_cast<float>(frame%113+3)/512.0f;}
inline fs::path vstBinary(const fs::path& module) {
    auto name=module.stem();
#if defined(_WIN32)
    name=module.filename();
#elif !defined(__APPLE__)
    name+=".so";
#endif
    fs::path result;
    for(const auto& entry:fs::recursive_directory_iterator(module))
        if(entry.is_regular_file() && entry.path().filename()==name) {
            require(result.empty(),"ambiguous owned fixture executable");result=entry.path();
        }
    require(!result.empty(),"owned fixture executable absent");return result;
}
inline void commit(Store& store,const char* type,Payload payload){OpRequest request;request.opType=type;request.payload=std::move(payload);
    const auto result=OpJournal(store).commit(request);require(result.ok,std::string(type)+": "+result.error);}
inline void seed(Store& store,const rows::PluginRef& ref,const fs::path& wave) {
    commit(store,"track.create",{{"id",1},{"kind","audio"},{"name","Source"}});
    commit(store,"track.create",{{"id",9},{"kind","master"},{"name","Master"},{"index",1}});
    SQLite::Statement plugin(store.db(),"INSERT INTO plugin_refs(id,format,uid,vendor,name,version,subtype,path_hint) VALUES(1,?,?, 'ADI','Owned fixture','1','effect',?)");
    plugin.bind(1,ref.format);plugin.bind(2,ref.uid);plugin.bind(3,ref.pathHint);plugin.exec();
    commit(store,"chain.create",{{"id",10},{"track",1}});
    commit(store,"device.insert",{{"id",100},{"chain",10},{"ord",0},{"ref",1},{"name","Saved hosted device"}});
    const auto hash=media::blake3File(wave);require(static_cast<bool>(hash),"source WAV hash");
    SQLite::Statement mediaRow(store.db(),"INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,?,'source.wav','wav')");
    mediaRow.bind(1,hash.hex);mediaRow.exec();
    SQLite::Statement clip(store.db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,?)");
    clip.bind(1,nanoseconds(8192));clip.exec();
    store.db().exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,11,8192)");
}
inline Json formatCase(const std::string& format) {
    TempDirectory scratch("session_catalogue_reopen",format.c_str());
    Json receipt{{"format",format},{"passed",false},{"sample_rate",48000},{"maximum_block",64},
        {"graph_transition_fade_frames",0},{"source","persisted Float32 stereo WAV clip"}};
    const auto project=scratch.path()/"saved.adi",wave=scratch.path()/"source.wav";
    scan::Config config;config.registry=scratch.path()/"registry.sqlite";config.executable=scratch.path()/"owned-scanner.exe";
    fs::copy_file(media::pathFromUtf8(ADI_CATALOGUE_SCANNER),config.executable);
    fs::create_directory(scratch.path()/"modules");fs::path module;
    if(format=="clap") {
        module=scratch.path()/"modules"/"runtime.clap";
#if defined(__APPLE__)
        const auto binary=module/"Contents"/"MacOS"/"runtime";fs::create_directories(binary.parent_path());
#else
        const auto binary=module;
#endif
        fs::copy_file(media::pathFromUtf8(ADI_CATALOGUE_CLAP),binary);
    } else {
        const auto original=media::pathFromUtf8(ADI_CATALOGUE_FIXTURE);module=scratch.path()/"modules"/original.filename();
        fs::copy(original,module,fs::copy_options::recursive);
    }
    rows::PluginRef ref;ref.format=format;std::string error;
    if(format=="clap") {
        auto scanner=scan::Scanner::open(config,error);require(static_cast<bool>(scanner),error);
        const auto result=scanner->probe(format,module);require(result.state==scan::State::Ready && result.plugins.size()==1,"real CLAP discovery: "+result.error);
        ref.uid=result.plugins[0].id;ref.pathHint=result.path;
    } else {
        device::Vst3Host host;juce::KnownPluginList types;
        const auto report=host.scanFile(juce::String::fromUTF8(text(module).c_str()),types,config);
        require(report.complete && report.failures.empty() && types.getNumTypes()==3,"real VST3 discovery");
        const auto description=types.getTypes().getReference(0);
        ref.uid=device::identityOf(description).uid;ref.pathHint=description.fileOrIdentifier.toStdString();
    }
    std::vector<float> pcm(8300*2);
    for(std::int64_t frame=0;frame<8300;++frame)for(int channel=0;channel<2;++channel)
        pcm[static_cast<std::size_t>(frame*2+channel)]=sample(frame,channel);
    {audio::WavWriter writer(wave,48000,2,audio::WavFormat::Float32);writer.write(pcm.data(),8300);writer.close();}
    StoreError storeError{};auto store=Store::createEmpty(project,storeError);require(static_cast<bool>(store),"create project");seed(*store,ref,wave);
    engine::SessionSpec spec;spec.channels=2;spec.sampleRate=48000;spec.maxFrames=64;spec.performanceState=false;
    std::vector<std::uint8_t> saved;std::string stateHash;double savedDrive=0,savedPatch=0;
    {
        device::JuceDeviceLoader loader;loader.setScanConfig(config);
        require(loader.hydrateCatalogue(error),"initial hydration: "+error);
        engine::Session session;session.graph().setFadeFrames(0);
        require(session.load(*store,loader.fn(),spec),"initial Session: "+session.error());
        require(session.stats().loaded==1 && session.stats().placeholders==0 && session.stats().statesLoaded==0,"initial actual hosted instance");
        auto* instance=session.instanceFor(100);require(instance && instance->loaded(),"initial device missing");
        engine::ParamOps capture;require(capture.attachSession(session)==1,"attach real state capture");
        if(format=="clap")require(instance->loadState("chunk",{'A','D','C','1',4,0,0,0}),"load real quarter-gain state");
        else {
            // The fixture preset changes state-only patch and Drive; neither is
            // a PCM gain in this VST3. Its no-note audio path is exact passthrough.
            require(instance->setParam("4003",device::ParamValue::fromNormalized(1)),"select real VST3 preset");
            savedDrive=instance->getParam("4000").normalized;savedPatch=instance->getParam("4004").normalized;
            // JUCE exposes normalized parameter values through float. Native
            // opaque state still has to survive byte-for-byte below.
            require(std::fabs(savedDrive-0.45)<1e-6 && std::fabs(savedPatch-7.0/127.0)<1e-6,
                "VST3 preset changed real controller state");
        }
        saved=instance->saveState("chunk");require(!saved.empty(),"nonempty native saved state");
        const auto digest=media::blake3Bytes(std::as_bytes(std::span(saved)));require(static_cast<bool>(digest),"state digest");stateHash=digest.hex;
        std::vector<OpRequest> requests;require(capture.snapshot(100,1000,requests)>0 && !requests.empty(),"snapshot offered no persistent state");
        const auto recorded=OpJournal(*store).commit(requests);require(recorded.ok,"record native state: "+recorded.error);
        require(requests.front().capturedTransaction && requests.front().capturedTransaction->phase()==CapturedSubmissionPhase::Durable,
            "state did not pass real captured-transaction persistence");
        SQLite::Statement state(store->db(),"SELECT state_hash FROM plugin_state WHERE device_id=100 AND stream_role='chunk'");
        require(state.executeStep() && state.getColumn(0).getString()==stateHash,"stored state row differs from actual snapshot");
        const auto blob=store->getStateBlob(stateHash);
        require(blob && blob->size()==saved.size() && std::memcmp(blob->data(),saved.data(),saved.size())==0,"stored state bytes differ");
        receipt["recorded"]={{"actual_instances",session.stats().loaded},{"state_bytes",saved.size()},
            {"state_blake3",stateHash},{"journal_requests",requests.size()},{"implicit_scans",loader.stats().scans}};
    }
    // All initial host/session/capture owners are gone before file closure.
    require(store->close(),"checkpoint and close original project");store.reset();
    require(fs::remove(config.executable),"remove owned helper before project reopen");
    require(!fs::exists(config.executable),"helper unexpectedly present");
    store=Store::open(project,storeError);require(static_cast<bool>(store) && !store->readOnly(),"reopen persisted project");
    {
        device::JuceDeviceLoader fresh;fresh.setScanConfig(config);require(fresh.hydrateCatalogue(error),"fresh cached hydration: "+error);
        require(fresh.stats().requests==0 && fresh.stats().scans==0 && fresh.clap().libraryCount()==0,"hydration performed runtime work");
        engine::Session session;session.graph().setFadeFrames(0);
        require(session.load(*store,fresh.fn(),spec),"reopened Session: "+session.error());
        require(session.stats().loaded==1 && session.stats().placeholders==0 && session.stats().statesLoaded==1,
            "Session did not instantiate and restore one actual hosted state");
        auto* instance=session.instanceFor(100);require(instance && instance->loaded() && instance->identity().uid==ref.uid,"saved hosted UID changed");
        require(instance->saveState("chunk")==saved,"actual restored native state differs from saved bytes");
        if(format=="vst3")require(instance->getParam("4000").normalized==savedDrive && instance->getParam("4004").normalized==savedPatch,
            "VST3 controller state did not restore");
        // Observe the module only after Session has instantiated it. A bypassed
        // passthrough VST3 would otherwise produce the same source PCM.
        juce::DynamicLibrary observedModule;
        using CompletedCalls=std::uint64_t(*)();CompletedCalls completedCalls=nullptr;
        if(format=="vst3") {
            require(observedModule.open(juce::String::fromUTF8(text(vstBinary(module)).c_str())),"observe loaded VST3 fixture");
            completedCalls=reinterpret_cast<CompletedCalls>(observedModule.getFunction("adiFixtureCompletedProcessCalls"));
            require(completedCalls && completedCalls()==0,"VST3 readiness counter absent or audio ran before first callback");
        }
        require(session.clips()!=nullptr,"persisted clip path absent");
        Json seeks=Json::array();std::int64_t stream=0;
        for(const auto [position,count]:std::array<std::pair<std::int64_t,int>,3>{{{31,19},{1003,64},{4097,1}}}) {
            session.transport().locate(position);session.transport().play();
            require(session.clips()->prime(std::chrono::seconds(10)),"owner clip priming failed");
            std::array<float,80> left,right;left.fill(-7);right.fill(-11);float* outputs[]{left.data()+4,right.data()+4};
            engine::AudioIo io;io.out=outputs;io.numOut=2;io.frames=count;io.streamTimeSamples=stream;
            const auto callsBefore=completedCalls?completedCalls():0;
            {audio::CallbackScope callback;session.process(io);}stream+=count;
            if(completedCalls)require(completedCalls()==callsBefore+1,"VST3 did not successfully process the Session block");
            bool exact=true;
            for(std::size_t i=0;i<left.size();++i){const bool inside=i>=4 && i<static_cast<std::size_t>(count+4);
                const auto source=position+static_cast<std::int64_t>(i)-4+11;const float gain=format=="clap"?0.25f:1.0f;
                exact=exact && left[i]==(inside?sample(source,0)*gain:-7.0f) && right[i]==(inside?sample(source,1)*gain:-11.0f);}
            require(exact,"first post-seek PCM or output guard differs");
            require(session.transport().position()==position+count && session.clips()->underrunSamples()==0,"reopen transport/clip underrun");
            seeks.push_back({{"position",position},{"frames",count},{"exact_first_pcm",exact},{"transport_after",session.transport().position()}});
            if(completedCalls)seeks.back()["completed_vst3_process_calls"]=completedCalls();
        }
        require(fresh.stats().scans==0 && fresh.stats().catalogueReads==1 && fresh.stats().requests==1 && !fs::exists(config.executable),
            "reopen performed discovery or recreated the helper");
        receipt["reopened"]={{"loaded",session.stats().loaded},{"statesLoaded",session.stats().statesLoaded},
            {"placeholders",session.stats().placeholders},{"catalogue_reads",fresh.stats().catalogueReads},
            {"implicit_scans",fresh.stats().scans},{"helper_absent",true},{"opaque_state_equal",true},{"seeks",seeks}};
    }
    require(store->close(),"close reopened project");store.reset();receipt["passed"]=true;return receipt;
}
} // namespace scanner_reopen_detail
inline bool scannerSessionReopenContracts() {
    juce::ScopedJuceInitialiser_GUI initialize;bool passed=true;
    for(const auto* format:{"clap","vst3"})try{const auto receipt=scanner_reopen_detail::formatCase(format);
        std::printf("SCANNER_SESSION_REOPEN %s\n",receipt.dump().c_str());passed=passed && receipt.at("passed").get<bool>();}
    catch(const std::exception& error){passed=false;std::printf("SCANNER_SESSION_REOPEN %s\n",
        scanner_reopen_detail::Json{{"format",format},{"passed",false},{"error",error.what()}}.dump().c_str());}
    return passed;
}
} // namespace adi::test
