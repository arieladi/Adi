// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal runtime CLAP fixture for persistent catalogue loading. No GUI/audio I/O.
#include <clap/clap.h>
#include <cstdint>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
namespace {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
std::array<std::atomic<std::uint64_t>,4> counters{}; // create, successful restore, process, frames
const char* const features[]{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,CLAP_PLUGIN_FEATURE_STEREO,nullptr};
const clap_plugin_descriptor_t description{CLAP_VERSION,"org.adi.catalogue.runtime","Catalogue runtime","ADI","","","","1","fixture",features};
struct Runtime {
    clap_plugin_t api{};
    std::uint8_t divisor=1;
    bool initialized=false,active=false,processing=false;
};
Runtime& runtime(const clap_plugin_t* plugin){return *static_cast<Runtime*>(plugin->plugin_data);}
bool CLAP_ABI init(const clap_plugin_t* plugin){runtime(plugin).initialized=true;return true;}
void CLAP_ABI destroy(const clap_plugin_t* plugin){delete &runtime(plugin);}
bool CLAP_ABI activate(const clap_plugin_t* plugin,double rate,std::uint32_t minimum,std::uint32_t maximum){
    auto& value=runtime(plugin);value.active=value.initialized && std::isfinite(rate) && rate>0 && minimum>0 && maximum>=minimum;return value.active;
}
void CLAP_ABI deactivate(const clap_plugin_t* plugin){auto& value=runtime(plugin);value.active=false;value.processing=false;}
void CLAP_ABI nothing(const clap_plugin_t*){}
bool CLAP_ABI start(const clap_plugin_t* plugin){auto& value=runtime(plugin);value.processing=value.active;return value.processing;}
void CLAP_ABI stop(const clap_plugin_t* plugin){runtime(plugin).processing=false;}
clap_process_status CLAP_ABI process(const clap_plugin_t* plugin,const clap_process_t* block){
    const auto& value=runtime(plugin);
    if(!block || !value.active || !value.processing || block->audio_inputs_count!=1 || block->audio_outputs_count!=1 ||
       !block->audio_inputs || !block->audio_outputs || block->audio_inputs[0].channel_count!=2 || block->audio_outputs[0].channel_count!=2 ||
       !block->audio_inputs[0].data32 || !block->audio_outputs[0].data32)return CLAP_PROCESS_ERROR;
    const auto gain=1.0f/value.divisor;
    for(unsigned channel=0;channel<2;++channel) {
        const auto* input=block->audio_inputs[0].data32[channel];auto* output=block->audio_outputs[0].data32[channel];
        if(!input || !output)return CLAP_PROCESS_ERROR;
        for(std::uint32_t frame=0;frame<block->frames_count;++frame)output[frame]=input[frame]*gain;
    }
    block->audio_outputs[0].constant_mask=0;
    counters[2].fetch_add(1,std::memory_order_relaxed);counters[3].fetch_add(block->frames_count,std::memory_order_relaxed);
    return CLAP_PROCESS_CONTINUE;
}
std::uint32_t CLAP_ABI portCount(const clap_plugin_t*,bool){return 1;}
bool CLAP_ABI portInfo(const clap_plugin_t*,std::uint32_t index,bool input,clap_audio_port_info_t* info){
    if(index!=0 || !info)return false;
    *info={};info->id=0;info->flags=CLAP_AUDIO_PORT_IS_MAIN;info->channel_count=2;info->port_type=CLAP_PORT_STEREO;info->in_place_pair=0;
    const auto* name=input?"Stereo input":"Stereo output";std::memcpy(info->name,name,std::strlen(name)+1);return true;
}
bool CLAP_ABI save(const clap_plugin_t* plugin,const clap_ostream_t* stream){
    if(!stream || !stream->write)return false;
    const std::array<std::uint8_t,8> bytes{'A','D','C','1',runtime(plugin).divisor,0,0,0};
    std::uint64_t offset=0;
    while(offset<bytes.size()) {
        const auto written=stream->write(stream,bytes.data()+offset,bytes.size()-offset);
        if(written<=0 || static_cast<std::uint64_t>(written)>bytes.size()-offset)return false;
        offset+=static_cast<std::uint64_t>(written);
    }
    return true;
}
bool CLAP_ABI load(const clap_plugin_t* plugin,const clap_istream_t* stream){
    if(!stream || !stream->read)return false;
    std::array<std::uint8_t,8> bytes{};std::uint64_t offset=0;
    while(offset<bytes.size()) {
        const auto got=stream->read(stream,bytes.data()+offset,bytes.size()-offset);
        if(got<=0 || static_cast<std::uint64_t>(got)>bytes.size()-offset)return false;
        offset+=static_cast<std::uint64_t>(got);
    }
    std::uint8_t extra=0;
    if(stream->read(stream,&extra,1)!=0 || std::memcmp(bytes.data(),"ADC1",4)!=0 || bytes[5]!=0 || bytes[6]!=0 || bytes[7]!=0 ||
       (bytes[4]!=1 && bytes[4]!=2 && bytes[4]!=4 && bytes[4]!=8))return false;
    runtime(plugin).divisor=bytes[4];counters[1].fetch_add(1,std::memory_order_relaxed);return true;
}
const clap_plugin_audio_ports_t ports{portCount,portInfo};
const clap_plugin_state_t state{save,load};
const void* CLAP_ABI extension(const clap_plugin_t*,const char* id){
    if(!id)return nullptr;
    if(std::strcmp(id,CLAP_EXT_AUDIO_PORTS)==0)return &ports;
    if(std::strcmp(id,CLAP_EXT_STATE)==0)return &state;
    return nullptr;
}
std::uint32_t CLAP_ABI count(const clap_plugin_factory_t*){return 1;}
const clap_plugin_descriptor_t* CLAP_ABI describe(const clap_plugin_factory_t*,std::uint32_t index){return index==0?&description:nullptr;}
const clap_plugin_t* CLAP_ABI create(const clap_plugin_factory_t*,const clap_host_t*,const char* id){
    if(std::strcmp(id,description.id)!=0)return nullptr;
    auto* value=new(std::nothrow) Runtime;
    if(!value)return nullptr;
    value->api={&description,value,init,destroy,activate,deactivate,start,stop,nothing,process,extension,nothing};
    counters[0].fetch_add(1,std::memory_order_relaxed);
    return &value->api;
}
const clap_plugin_factory_t factory{count,describe,create};
bool CLAP_ABI entry(const char* path){
    std::ofstream log(std::filesystem::path(std::u8string(path,path+std::strlen(path))) += ".pids",std::ios::app);
#if defined(_WIN32)
    log<<_getpid()<<'\n';
#else
    log<<getpid()<<'\n';
#endif
    return true;
}
void CLAP_ABI leave(){}
const void* CLAP_ABI getFactory(const char* id){return std::strcmp(id,CLAP_PLUGIN_FACTORY_ID)==0?&factory:nullptr;}
}
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION,entry,leave,getFactory};
// Test-owned observation only. Reading never mutates host or plug-in state.
extern "C" CLAP_EXPORT std::uint64_t adiCatalogueReadCounter(std::uint32_t index){return index<counters.size()?counters[index].load(std::memory_order_relaxed):0;}
