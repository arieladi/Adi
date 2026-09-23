# CLAP host contract audit — 2026-09-23

Auditor: linux (Codex), under win's 2026-09-23 grant. Reviewed host and pinned
CLAP headers at main `9e8d561`; the relevant files are unchanged by #70.
Gemini CLI was attempted twice with all tools denied and source text on stdin,
but returned `IneligibleTierError` (installed client no longer supported).
There is **no Gemini analysis** here. No repository credentials were supplied.
This is a direct read-only audit, not a host patch or a new platform feature.

## Verified findings for win

Each numbered mode of the standalone probe below links **unchanged adi_core**.
Every mode compiled under GCC 15.2.0 and Clang 21.1.8 and exited **1 at its
contract assertion**, with the outputs shown. A failing assertion is the
reproduction of the defect being reported, not a failing shipping test.
Severity is linux's triage proposal; win owns the fix and sequencing.

| ID / severity | Header contract and implementation lines | Observed result / failing test |
|---|---|---|
| C1 / high | `third_party/clap/include/clap/ext/audio-ports.h:102–111` exposes `is_rescan_flag_supported`; `src/juce/clap_host.cpp:1053–1054,1065` advertises the struct but initializes only `rescan`. | Probe 1: `advertised=1 callback=0`. Assert that an advertised audio-port extension supplies its query callback. A plugin calling it can dereference null; the probe checks the pointer without crashing. |
| C2 / high | `plugin.h:75–97`: process requires processing state, start returns success; `clap_host.cpp:633–634,764–766` discards that result and gates only on activation. | Probe 2: `start_success=0 process_calls=1`. Assert no process after failed start. |
| C3 / high | `plugin.h:60–71`: process frames must be within activation's min/max; `clap_host.cpp:630–632` passes maxFrames as both bounds, while `:768,869` forwards smaller graph segments. | Probe 3: `advertised=[512,512] actual=128`. Assert actual frame count falls within the recorded activation bounds. Fix must preserve granted maximum while permitting segmentation; no ADR edited here. |
| C4 / medium | `ext/audio-ports.h:67–79`: scan while deactivated; `clap_host.cpp:630–671` activates then scans count/get. `:610–611,705–729` also scans to test a same-format active layout. | Probe 4: `active_port_queries=4` on first prepare. Assert zero active scans. The first-prepare path is reproduced; same-format path is additionally visible statically. |
| C5 / medium | `ext/params.h:16–19,48–50,290–305`: nonprocessing parameter changes can use flush; `clap_host.cpp:436–456` only queues, and the queue is first allocated at `:695`. | Probe 5: before prepare, a known parameter returns `accepted=0 flushes=0 value=0.25` after attempting 0.8. Assert an accepted/applied nonprocessing edit. This verifies the missing pre-prepare edit path, not a claim that flush is mandatory during process. Session's mirror application calls `setParam` at `src/adi/engine/session.cpp:184`; no Session change made. |
| C6 / high | `ext/latency.h:12–14`: get is main-thread; `src/adi/engine/graph.cpp:850–852` → `src/juce/device_model.cpp:182–192` → `clap_host.cpp:555–578` calls it while forwarding events on the audio thread. | Probe 6: `audio_latency_queries=1 callback_allocations=1`. Assert neither main-thread-only query nor allocation occurs in Graph::process. The fake getter deliberately allocates, legal on its documented main thread. See [audio-thread audit](audio-thread.md). |

Paths in the header column abbreviate `third_party/clap/include/clap/`;
implementation `clap_host.cpp` abbreviates `src/juce/clap_host.cpp`. Line numbers
refer to the pinned reviewed revision, not a future edited file.

## Coverage and exclusions

- `ClapLibrary::create` (`clap_host.cpp:1025–1031`) initializes before the
  `ClapDevice` extension queries (`:337–349`) and destroys after failed init.
  Destructor/release normally stop, deactivate, destroy (`:366–376,746–753`),
  but C2 shows that activated and successfully processing are conflated.
- `ext/thread-check.h:33–37` explicitly allows main OS thread to act as the
  symbolic audio thread when serialized. Merely seeing start/stop in prepare
  is **not** proof of a thread violation. No such finding is claimed.
- Params info/value calls are consumer-side. Missing `clap.params` host
  negotiation and discarded output events (`clap_host.cpp:756–761`) are
  integration gaps already owned by win's parameter glue, not new completed
  functionality inferred from ParamEditCapture.
- State save/load (`clap_host.cpp:511–524`) use the optional extension and
  main-thread methods (`ext/state.h:24–33`). Existing `adi_clap_tests` checks
  bytes and incomplete extension pointers. No new state defect was reproduced;
  arbitrary plugin exceptions and allocation failure during streams were not
  injected, and are not certified safe by this audit.
- Note-port dialect negotiation and unknown extension null results were read;
  the required callback missing in C1 is distinguished from optional extensions
  the host honestly declines. No real plugin, JUCE, GUI or audio driver was used.

## Reproduction

Copy the C++ block below to `/tmp/adi-contract-probe.cpp`. From the repository
root, with a completed headless GCC build at `$BUILD`:

```bash
g++ -std=c++20 -O2 -pthread -I adi_daw/src \
  -I adi_daw/third_party/clap/include /tmp/adi-contract-probe.cpp \
  "$BUILD/libadi_core.a" -ldl -o /tmp/adi-contract-probe
# Each intentionally exits 1 on the reviewed revision:
for mode in 1 2 3 4 5 6; do /tmp/adi-contract-probe "$mode"; done
```

Use the Clang-built library and `clang++` for the second compiler. This probe
uses the existing `check`-style boolean/exit-code approach; no new framework,
CMake target, engine mutation or production source edit is needed. Raw outputs
are retained in the originating task's `work/gemini-audits/probe-{gcc,clang}-N.log`.

```cpp
// Standalone read-only audit probe; links unchanged adi_core.
#include "juce/clap_host.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
using namespace adi::device;
using namespace adi::engine;
static std::atomic<int> allocations{0};
static std::atomic<bool> countAllocations{false};
void* operator new(std::size_t n) {
    if (countAllocations.load()) ++allocations;
    auto* p=std::malloc(n?n:1); if(!p) throw std::bad_alloc(); return p;
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
struct Plugin {
    clap_plugin_t api{}; clap_plugin_audio_ports_t ports{};
    clap_plugin_params_t params{}; clap_plugin_latency_t latency{};
    bool active=false, startOk=true, allocatingLatency=false;
    int processCalls=0, activeScans=0, flushes=0, audioLatency=0;
    std::uint32_t minimum=0, maximum=0, actual=0;
    double value=0.25;
    std::thread::id main=std::this_thread::get_id();
    static Plugin& self(const clap_plugin_t* p) {return *static_cast<Plugin*>(p->plugin_data);}
    Plugin() {
        api.plugin_data=this;
        api.init=[](const clap_plugin_t*){return true;};
        api.destroy=[](const clap_plugin_t*){};
        api.activate=[](const clap_plugin_t* p,double,std::uint32_t lo,std::uint32_t hi){auto& s=self(p);s.active=true;s.minimum=lo;s.maximum=hi;return true;};
        api.deactivate=[](const clap_plugin_t* p){self(p).active=false;};
        api.start_processing=[](const clap_plugin_t* p){return self(p).startOk;};
        api.stop_processing=[](const clap_plugin_t*){};
        api.process=[](const clap_plugin_t* p,const clap_process_t* b)->clap_process_status {
            auto& s=self(p);++s.processCalls;s.actual=b->frames_count;
            for(std::uint32_t ch=0;ch<b->audio_outputs[0].channel_count;++ch)
                for(std::uint32_t i=0;i<b->frames_count;++i)b->audio_outputs[0].data32[ch][i]=0.5f;
            return CLAP_PROCESS_CONTINUE;
        };
        api.get_extension=[](const clap_plugin_t* p,const char* id)->const void* {
            auto& s=self(p);
            if(!std::strcmp(id,CLAP_EXT_AUDIO_PORTS))return &s.ports;
            if(!std::strcmp(id,CLAP_EXT_PARAMS))return &s.params;
            if(!std::strcmp(id,CLAP_EXT_LATENCY))return &s.latency;
            return nullptr;
        };
        ports.count=[](const clap_plugin_t* p,bool)->std::uint32_t {auto& s=self(p);if(s.active)++s.activeScans;return 1;};
        ports.get=[](const clap_plugin_t* p,std::uint32_t,bool,clap_audio_port_info_t* i){auto& s=self(p);if(s.active)++s.activeScans;*i={};i->channel_count=2;return true;};
        params.count=[](const clap_plugin_t*)->std::uint32_t{return 1;};
        params.get_info=[](const clap_plugin_t*,std::uint32_t,clap_param_info_t* i){*i={};i->id=1;i->min_value=0;i->max_value=1;i->default_value=0.25;return true;};
        params.get_value=[](const clap_plugin_t* p,clap_id,double* v){*v=self(p).value;return true;};
        params.flush=[](const clap_plugin_t* p,const clap_input_events_t* in,const clap_output_events_t*){
            auto& s=self(p);++s.flushes;for(std::uint32_t n=0;n<in->size(in);++n){const auto* h=in->get(in,n);if(h->type==CLAP_EVENT_PARAM_VALUE)s.value=reinterpret_cast<const clap_event_param_value_t*>(h)->value;}
        };
        latency.get=[](const clap_plugin_t* p)->std::uint32_t {
            auto& s=self(p);
            if(std::this_thread::get_id()!=s.main)++s.audioLatency;
            if(s.allocatingLatency){void* q=::operator new(8);::operator delete(q);}
            return 0;
        };
    }
};
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    const int mode=std::atoi(argv[1]);bool pass=false;
    if(mode==1){ClapHostGlue glue;const auto* h=glue.host();const auto* e=static_cast<const clap_host_audio_ports_t*>(h->get_extension(h,CLAP_EXT_AUDIO_PORTS));pass=e&&e->is_rescan_flag_supported;std::printf("advertised=%d callback=%d\n",e!=nullptr,pass);}
    else {
        Plugin f; f.api.init(&f.api); f.startOk=mode!=2; ClapDevice d(&f.api,{});
        float l[512]{},r[512]{};float* out[]={l,r};NodeIo io;io.out=out;io.channels=2;io.frames=128;
        if(mode==5){const bool accepted=d.setParam("00000001",ParamValue::withReal(0.8,0.8));pass=accepted&&f.value==0.8;std::printf("accepted=%d flushes=%d value=%g\n",accepted,f.flushes,f.value);}
        else if(mode==6){
            DeviceNode node(d);SumNode sum;Graph graph;auto a=graph.addNode(node),b=graph.addNode(sum);graph.connect(a,b);graph.setOutput(b);graph.prepare(48000,512);
            Event e{};e.type=EventType::NoteOn;e.noteId=1;e.dim=60;e.value=1;graph.pushInputEvent(a,e);
            f.allocatingLatency=true;
            AudioIo audio;audio.out=out;audio.numOut=2;audio.frames=512;
            std::thread t([&]{countAllocations=true;graph.process(audio);countAllocations=false;});t.join();
            pass=f.audioLatency==0&&allocations==0;std::printf("audio_latency_queries=%d callback_allocations=%d\n",f.audioLatency,allocations.load());
        } else {d.prepare(48000,512);d.process(io);
            if(mode==2){pass=f.processCalls==0;std::printf("start_success=0 process_calls=%d\n",f.processCalls);}
            if(mode==3){pass=f.actual>=f.minimum&&f.actual<=f.maximum;std::printf("advertised=[%u,%u] actual=%u\n",f.minimum,f.maximum,f.actual);}
            if(mode==4){pass=f.activeScans==0;std::printf("active_port_queries=%d\n",f.activeScans);}
        }
    }
    std::printf("%s contract probe %d\n",pass?"PASS":"FAIL",mode);return pass?0:1;
}

```
