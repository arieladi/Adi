// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/plugin_scanner.hpp"
#include "adi/scan_helper_protocol.hpp"
#include "temp_directory.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::scan;
namespace fs=std::filesystem;
using Json=nlohmann::json;
int checks=0,failures=0;
void check(bool ok,const char* text) {
    ++checks;if(!ok){++failures;std::printf("FAIL: %s\n",text);}
}
std::string utf8(const fs::path& path) {const auto bytes=path.u8string();return {bytes.begin(),bytes.end()};}
fs::path native(const std::string& bytes) {return fs::path(std::u8string(bytes.begin(),bytes.end()));}
unsigned initializations(const fs::path& marker) {
    std::ifstream in(marker);long long pid=0;unsigned count=0;
    while(in>>pid)++count;
    return count;
}
void run() {
    test::TempDirectory temp("scan_self_test","native");
    const auto helper=native(ADI_SCAN_HELPER);
    const auto candidate=temp.path()/native("owned caf\xc3\xa9 no-load.clap");
#if defined(__APPLE__)
    const auto binary=candidate/"Contents"/"MacOS"/candidate.stem();
    fs::create_directories(binary.parent_path());
#else
    const auto binary=candidate;
#endif
    fs::copy_file(native(ADI_SCAN_FIXTURE),binary);
    const auto marker=native(utf8(candidate)+".pids");
    ProcessOptions options;options.timeout=std::chrono::milliseconds(5000);
    options.maximumOutput=kMaximumHelperSelfTestPacket;
    const auto observed=runProcess(helper,{"--self-test"},options);
    check(observed.succeeded(),"actual dedicated helper accepts exact --self-test command");
    if(!observed.succeeded())return;
    check(!observed.output.empty() && observed.output.back()=='\n' &&
          std::count(observed.output.begin(),observed.output.end(),'\n')==1 &&
          observed.output.size()<=kMaximumHelperSelfTestPacket,"self-test emits one bounded JSON record without diagnostic chatter");
    const auto packet=Json::parse(observed.output);
    Json expectedFormats=Json::array({"clap"});
#if defined(ADI_EXPECT_SCAN_VST3)
    expectedFormats.push_back("vst3");
#endif
    const Json expected={{"kind",kHelperSelfTestKind},{"protocol",kHelperSelfTestProtocol},
        {"metadata_schema",kPacketSchema},{"formats",expectedFormats},
        {"evidence","structural"},{"plugin_loaded",false}};
    check(packet==expected,"owned protocol reports exactly its version, metadata schema, compiled formats and structural evidence");
    check(!fs::exists(marker),"self-test performs no initialization of the owned candidate");
    const std::vector<std::vector<std::string>> malformed{
        {},{"--SELF-TEST"},{"--self-test=1"},{"--self-test","extra"},
        {"--self-test","clap",utf8(candidate)},
        {"--self-test","--scan","clap",utf8(candidate)},
        {"--scan","clap",utf8(candidate),"--self-test"}};
    for(const auto& arguments:malformed) {
        const auto result=runProcess(helper,arguments,options);
        check(result.status==ProcessStatus::Exited && result.exitCode==64 && result.output.empty(),
              "malformed or mixed self-test arguments refuse with usage status and no protocol output");
    }
    check(!fs::exists(marker),"refused mixed self-test/scan commands cannot reach plug-in loading");

    // The explicit diagnostic is separate from discovery. Real CLAP probing
    // retains its one initialization over three unchanged calls/reopen.
    Config config;config.executable=helper;config.registry=temp.path()/"registry.sqlite";
    config.process.timeout=std::chrono::milliseconds(5000);
    std::string error;auto scanner=Scanner::open(config,error);
    check(scanner!=nullptr,"ordinary discovery still opens independently of self-test");
    if(!scanner)return;
    const auto cold=scanner->probe("clap",candidate);
    check(cold.state==State::Ready && !cold.cacheHit && cold.plugins.size()==2 && initializations(marker)==1,
          "positive control proves the actual helper loads the owned CLAP exactly once when explicitly scanned");
    const auto warm=scanner->probe("clap",candidate);
    scanner.reset();scanner=Scanner::open(config,error);
    if(!scanner)throw std::runtime_error(error);
    const auto reopened=scanner->probe("clap",candidate);
    check(warm.state==State::Ready && warm.cacheHit && reopened.state==State::Ready && reopened.cacheHit &&
          initializations(marker)==1,"three unchanged actual-helper probes including reopen retain one module initialization");
    const auto again=runProcess(helper,{"--self-test"},options);
    auto registry=PluginRegistry::open(config.registry,error);
    const auto blocks=registry?registry->scanBlocks(error):std::nullopt;
    check(again.succeeded() && again.output==observed.output && initializations(marker)==1 && blocks && blocks->empty(),
          "explicit self-test stays deterministic and does not load the candidate or poison quarantine");
    std::printf("SELF_TEST_PACKET %s",observed.output.c_str());
}
} // namespace
int main() {
    try {run();}catch(const std::exception& error){++failures;std::printf("EXCEPTION: %s\n",error.what());}
    std::printf("%s -- helper self-test: %d checks, %d failure(s)\n",failures?"FAILED":"PASS",checks,failures);
    return failures?1:0;
}
