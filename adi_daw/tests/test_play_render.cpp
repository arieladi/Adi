// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the exact production renderer and WAV writer, without a device.
#define main adi_play_command_main
#include "../src/juce/play.cpp"
#undef main
#include "adi/media/media_ops.hpp"
#include "adi/scan_catalogue.hpp"
#include "temp_directory.hpp"
#include "legacy_plugin_registry.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <fstream>

#if defined(ADI_CATALOGUE_FIXTURE)
#include "scanner_readiness.hpp"
#include "scanner_session_reopen.hpp"
#include "scanner_discovery.hpp"
#include "scanner_search_settings.hpp"
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
bool persistentCatalogueContracts() {
    namespace fs=std::filesystem;
    const auto text=[](const fs::path& path){const auto value=path.u8string();return std::string(value.begin(),value.end());};
    const auto check=[](bool ok,const char* label){if(!ok)std::printf("FAIL -- %s\n",label);return ok;};
    juce::ScopedJuceInitialiser_GUI initialize;
    adi::test::TempDirectory scratch("play_render","persistent_catalogue");
    adi::scan::Config config;config.registry=scratch.path()/"registry.sqlite";
    config.executable=scratch.path()/"owned-scanner.exe";
    fs::copy_file(adi::media::pathFromUtf8(ADI_CATALOGUE_SCANNER),config.executable);
    const auto clapPath=scratch.path()/"runtime.clap";
#if defined(__APPLE__)
    fs::create_directories(clapPath/"Contents"/"MacOS");
    fs::copy_file(adi::media::pathFromUtf8(ADI_CATALOGUE_CLAP),clapPath/"Contents"/"MacOS"/"runtime");
#else
    fs::copy_file(adi::media::pathFromUtf8(ADI_CATALOGUE_CLAP),clapPath);
#endif
    adi::device::Vst3Host discovery;juce::KnownPluginList vstTypes;
    // The persisted catalogue stores canonical paths, including for symlinked build trees.
    const auto vstPath=text(fs::canonical(adi::media::pathFromUtf8(ADI_CATALOGUE_FIXTURE)));
    const auto discovered=discovery.scanFile(juce::String::fromUTF8(vstPath.c_str()),vstTypes,config);
    if(!check(discovered.complete && discovered.failures.empty() && vstTypes.getNumTypes()==3,"real VST3 catalogue population"))return false;
    std::string error;auto scanner=adi::scan::Scanner::open(config,error);
    if(!check(scanner && scanner->probe("clap",clapPath).state==adi::scan::State::Ready,"real CLAP catalogue population"))return false;
    scanner.reset();const auto stored=adi::scan::readCatalogue(config.registry,error);
    if(!check(stored && stored->pluginCount==4,"four persisted real fixture descriptions"))return false;
    const auto clapCanonical=text(fs::canonical(clapPath));
    const auto* vstModule=stored->find("vst3",vstPath);const auto* clapModule=stored->find("clap",clapCanonical);
    if(!vstModule || !clapModule)return false;
    const auto vstPacket=adi::scan::encodeMetadata("vst3",vstPath,vstModule->plugins,error);
    const auto clapPacket=adi::scan::encodeMetadata("clap",clapCanonical,clapModule->plugins,error);
    if(!vstPacket || !clapPacket || !fs::remove(config.executable))return false;
    const auto description=vstTypes.getTypes().getReference(0);
    adi::rows::PluginRef vstRef;vstRef.format="vst3";vstRef.uid=adi::device::identityOf(description).uid;
    vstRef.pathHint=description.fileOrIdentifier.toStdString();
    adi::rows::PluginRef clapRef;clapRef.format="clap";clapRef.uid="org.adi.catalogue.runtime";clapRef.pathHint=clapCanonical;
    const auto load=[](adi::device::JuceDeviceLoader& loader,const adi::rows::PluginRef& ref){
        adi::engine::DeviceRequest request;request.ref=&ref;std::string message;
        auto instance=loader.fn()(request,message);
        return instance && instance->loaded() && instance->identity().uid==ref.uid;
    };
    for(unsigned round=0;round<2;++round) {
        adi::device::JuceDeviceLoader loader;loader.setScanConfig(config);
        if(!check(load(loader,vstRef) && load(loader,clapRef) && loader.stats().scans==0 && loader.stats().catalogueReads==1,
            "fresh loader/reopen instantiates real VST3 and CLAP from cache with helper absent"))return false;
        auto noHint=vstRef;noHint.pathHint.clear();
        if(!check(load(loader,noHint) && loader.stats().catalogueReads==1,"known cached UID needs no hint or repeated SQL hydration"))return false;
        auto canonical = noHint;
        canonical.uid = "AD1E00014D5045319F2A0C1100000001";
        adi::rows::Device remote;
        remote.remoteHostId = 7;
        adi::engine::DeviceRequest fallbackRequest;
        fallbackRequest.device = &remote;
        fallbackRequest.ref = &canonical;
        std::string fallbackError;
        auto fallback = loader.fn({}, [&](std::string_view uid) -> std::optional<std::string> {
            if (uid == canonical.uid) return vstRef.uid;
            return {};
        })(fallbackRequest, fallbackError);
        if (!check(fallback && fallback->loaded() && fallback->identity().uid == canonical.uid &&
                   fallbackError.find("playing the local plug-in") != std::string::npos,
                   "unavailable server falls back through the local class ID mapping")) return false;
        // A deliberately bad root/target must not reach enumeration or helper
        // discovery once a valid persistent snapshot has been installed.
        const auto trap=scratch.path()/"root-trap";fs::create_directories(trap);
        std::ofstream(trap/"bad.vst3")<<"not a plug-in";
        loader.addSearchPath(juce::File(juce::String::fromUTF8(text(trap).c_str())));
        auto otherLocation=description;otherLocation.fileOrIdentifier=juce::File(
            juce::String::fromUTF8(text(trap/"bad.vst3").c_str())).getFullPathName();
        auto unknown=vstRef;unknown.uid=otherLocation.createIdentifierString().toStdString();
        unknown.pathHint=otherLocation.fileOrIdentifier.toStdString();
        // SC-324: a project saved where the plug-in stood at another path (another computer, a moved
        // folder) finds it by name and class ID, keeps its saved identity, and loads the catalogued module;
        // the bad file at the saved path is never scanned or loaded.
        if(!check(load(loader,unknown) && loader.stats().moved==1 &&
            loader.stats().scans==0 && loader.stats().needsDiscovery==0 && loader.stats().catalogueReads==1,
            "a VST3 saved at another path loads from the catalogue by its name and class ID, no helper or root scan"))return false;
        auto noHintMoved=unknown;noHintMoved.pathHint.clear();
        {
            adi::engine::DeviceRequest request;request.ref=&noHintMoved;
            auto instance=loader.fn()(request,error);
            if(!check(instance && instance->loaded() && instance->identity().uid==vstRef.uid && loader.stats().moved==2,
                "with no saved path either, it is found, under this machine's identity"))return false;
        }
        auto otherClass=otherLocation;otherClass.uniqueId^=0x5a5a;otherClass.deprecatedUid^=0x5a5a;
        auto stranger=unknown;stranger.uid=otherClass.createIdentifierString().toStdString();
        adi::engine::DeviceRequest request;request.ref=&stranger;
        if(!check(!loader.fn()(request,error) && error.find("NeedsDiscovery")!=error.npos &&
            loader.stats().scans==0 && loader.stats().needsDiscovery==1 && loader.stats().catalogueReads==1,
            "another class ID at another path is not guessed: NeedsDiscovery, no helper or root scan"))return false;
        const auto reads=loader.stats().catalogueReads;error="untouched";bool refused=false;
        {adi::audio::CallbackScope scope;refused=!loader.hydrateCatalogue(error);}
        if(!check(refused && error=="untouched" && loader.stats().catalogueReads==reads,"catalogue hydration refuses callbacks before mutation"))return false;
    }
    {
        auto alias=fs::path(ADI_CATALOGUE_FIXTURE);std::error_code ec;
        const auto link=scratch.path()/"alias.vst3";fs::create_directory_symlink(alias,link,ec);
        if(!ec)alias=link;
#if defined(_WIN32)
        else {auto upper=text(alias);for(char& ch:upper)if(ch>='a' && ch<='z')ch=static_cast<char>(ch-'a'+'A');alias=adi::media::pathFromUtf8(upper);}
#endif
        auto projected=description;projected.fileOrIdentifier=juce::File(juce::String::fromUTF8(text(alias).c_str())).getFullPathName();
        auto ref=vstRef;ref.uid=projected.createIdentifierString().toStdString();ref.pathHint=projected.fileOrIdentifier.toStdString();
        adi::device::JuceDeviceLoader loader;loader.setScanConfig(config);
        if(!check(load(loader,ref) && loader.stats().scans==0,"same canonical module projects saved alias spelling without rewriting its exact VST3 UID"))return false;
        if(!ec)fs::remove(link);
    }
    auto registry=adi::PluginRegistry::open(config.registry,error);if(!registry)return false;
    SQLite::Database db(text(config.registry),SQLite::OPEN_READWRITE);
    auto bad=vstModule->plugins;bad.back().data="<PLUGIN/>";
    const auto malformed=adi::scan::encodeMetadata("vst3",vstPath,bad,error);if(!malformed)return false;
    const auto replacePacket=[&](const std::string& value){SQLite::Statement update(db,"UPDATE plugin_scan_cache SET packet=? WHERE format='vst3'");update.bind(1,value);update.exec();};
    replacePacket(*malformed);
    {
        adi::device::JuceDeviceLoader loader;loader.setScanConfig(config);
        if(!check(!loader.hydrateCatalogue(error) && loader.knownPlugins().empty() && !load(loader,clapRef) && loader.clap().libraryCount()==0,
            "bad later VST3 XML prevents partial publication and fails closed even for earlier staged CLAP metadata"))return false;
    }
    replacePacket(*vstPacket);
    registry->blockScan("vst3",vstPath,"manual fixture block",124,error);
    {
        adi::device::JuceDeviceLoader loader;loader.setScanConfig(config);
        if(!check(loader.hydrateCatalogue(error) && !load(loader,vstRef) && load(loader,clapRef),
            "fresh hydration excludes quarantined VST3 while healthy CLAP still loads"))return false;
    }
    registry->unblockScan("vst3",vstPath,error);auto ticket=registry->beginScan("vst3",vstPath,125,error);
    if(!ticket || !registry->finishCachedScan(*ticket,vstModule->stamp,*vstPacket,126,error))return false;
    ticket=registry->beginScan("clap",clapCanonical,125,error);
    if(!ticket || !registry->finishFailedScan(*ticket,clapModule->stamp,adi::ScanFailureKind::Rejected,"negative fixture",126,error))return false;
    {
        adi::device::JuceDeviceLoader loader;loader.setScanConfig(config);
        if(!check(loader.hydrateCatalogue(error) && !load(loader,clapRef) && load(loader,vstRef) && registry->scanBlocks(error)->empty(),
            "negative CLAP metadata is excluded without quarantine or damaging healthy VST3 hydration"))return false;
    }
    std::ifstream pids(adi::media::pathFromUtf8(clapCanonical+".pids"));long long pid=0;unsigned foreign=0,runtime=0;
    while(pids>>pid){if(pid==static_cast<long long>(adi::test::processId()))++runtime;else ++foreign;}
    if(!check(foreign==1 && runtime>=3,"CLAP receipt has one discovery child followed only by runtime instantiation in the test parent"))return false;
    return true;
}
bool legacyCatalogueContracts() {
    namespace fs=std::filesystem;
    const auto text=[](const fs::path& path){const auto bytes=path.u8string();return std::string(bytes.begin(),bytes.end());};
    const auto read=[](const fs::path& path){std::ifstream in(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
    const auto check=[](bool ok,const char* label){if(!ok)std::printf("FAIL -- %s\n",label);return ok;};
    juce::ScopedJuceInitialiser_GUI initialize;
    adi::test::TempDirectory scratch("play_render","legacy_catalogue");
    adi::scan::Config seed;seed.registry=scratch.path()/"seed.sqlite";
    seed.executable=adi::media::pathFromUtf8(ADI_CATALOGUE_SCANNER);
    adi::device::Vst3Host discovery;juce::KnownPluginList types;
    const auto discovered=discovery.scanFile(ADI_CATALOGUE_FIXTURE,types,seed);
    if(!discovered.complete || !discovered.failures.empty() || types.getNumTypes()!=3)return false;
    const auto description=types.getTypes().getReference(0);
    adi::rows::PluginRef ref;ref.format="vst3";ref.uid=adi::device::identityOf(description).uid;
    ref.pathHint=description.fileOrIdentifier.toStdString();
    adi::engine::DeviceRequest request;request.ref=&ref;std::string error;
    for(const int version:{1,2})for(const bool explicitHydration:{false,true}) {
        auto config=seed;config.registry=scratch.path()/("old-"+std::to_string(version)+(explicitHydration?"-project":"-lazy")+".sqlite");
        adi::test::createLegacyPluginRegistry(config.registry,version);
        if(version==2) {
            SQLite::Database db(text(config.registry),SQLite::OPEN_READWRITE);
            SQLite::Statement block(db,"INSERT INTO plugin_scan_blocks(format,path,state,reason,updated_utc) VALUES('clap',?,'blocked','preserve old block',43)");
            block.bind(1,text(scratch.path()/"other.clap"));block.exec();
        }
        const auto before=read(config.registry);
        adi::device::JuceDeviceLoader loader;loader.setScanConfig(config);
        // Same explicit owner boundary called by GUI and CLI before project open.
        if(explicitHydration && !check(loader.hydrateCatalogue(error) && error.empty() &&
            read(config.registry)==before && loader.knownPlugins().empty(),"project-open hydration recognizes genuine legacy schema without migration"))return false;
        auto device=loader.fn()(request,error);
        if(!check(device && device->loaded() && device->identity().uid==ref.uid && loader.stats().needsDiscovery==0,
            "genuine v1/v2 registry keeps legacy saved-hint discovery and real instance creation"))return false;
        device.reset();SQLite::Database db(text(config.registry),SQLite::OPEN_READONLY);
        if(!check(db.execAndGet("PRAGMA user_version").getInt()==adi::PluginRegistry::kVersion &&
            db.execAndGet("SELECT updated_utc FROM plugin_routes WHERE uid='legacy.preference'").getInt()==42 &&
            db.execAndGet("SELECT count(*) FROM plugin_scan_cache").getInt()==1 &&
            db.execAndGet("SELECT count(*) FROM plugin_scan_blocks").getInt()==(version==2?1:0),
            "only actual owner discovery migrates legacy routes/quarantine and persists validated cache"))return false;
    }
    for(const int mode:{0,1,2}) {
        auto config=seed;config.registry=scratch.path()/("bad-"+std::to_string(mode)+".sqlite");
        adi::test::createLegacyPluginRegistry(config.registry,2);
        {SQLite::Database db(text(config.registry),SQLite::OPEN_READWRITE);
            if(mode==0)db.exec("PRAGMA application_id=1");
            else if(mode==1)db.exec("PRAGMA user_version=4");
            else db.exec("ALTER TABLE plugin_scan_blocks RENAME COLUMN state TO bad_state");}
        const auto before=read(config.registry);adi::device::JuceDeviceLoader loader;loader.setScanConfig(config);
        if(!check(!loader.hydrateCatalogue(error) && !loader.fn()(request,error) && loader.stats().scans==0 &&
            read(config.registry)==before,"foreign/newer/corrupt pre-cache-looking registry fails closed even with working helper and valid hint"))return false;
    }
    return true;
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
    if(!persistentCatalogueContracts())return 1;
    std::printf("PASS -- persistent catalogue hydrates real VST3/CLAP without helper and fails closed on corrupt/blocked metadata\n");
    if(!legacyCatalogueContracts())return 1;
    std::printf("PASS -- genuine legacy registries defer migration to discovery; invalid stores remain closed\n");
    if(!adi::test::scannerReadinessContracts())return 1;
    if(!adi::test::scannerSessionReopenContracts())return 1;
    if(!adi::test::scannerDiscoveryContracts())return 1;
    if(!adi::test::scannerSearchSettingsContracts())return 1;
#endif
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
        while(std::getline(csv,line))++lines;
        if(lines!=3)return 1;
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
        spec.offline = true;
        spec.performanceState = false;
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
