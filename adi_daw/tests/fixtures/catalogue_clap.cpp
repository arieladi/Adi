// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal runtime CLAP fixture for persistent catalogue loading. No GUI/audio I/O.
#include <clap/clap.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
namespace {
const clap_plugin_descriptor_t description{CLAP_VERSION,"org.adi.catalogue.runtime","Catalogue runtime","ADI","","","","1","fixture",nullptr};
bool CLAP_ABI init(const clap_plugin_t*){return true;}
void CLAP_ABI destroy(const clap_plugin_t* plugin){delete plugin;}
bool CLAP_ABI activate(const clap_plugin_t*,double,std::uint32_t,std::uint32_t){return true;}
void CLAP_ABI nothing(const clap_plugin_t*){}
bool CLAP_ABI start(const clap_plugin_t*){return true;}
clap_process_status CLAP_ABI process(const clap_plugin_t*,const clap_process_t*){return CLAP_PROCESS_CONTINUE;}
const void* CLAP_ABI extension(const clap_plugin_t*,const char*){return nullptr;}
std::uint32_t CLAP_ABI count(const clap_plugin_factory_t*){return 1;}
const clap_plugin_descriptor_t* CLAP_ABI describe(const clap_plugin_factory_t*,std::uint32_t index){return index==0?&description:nullptr;}
const clap_plugin_t* CLAP_ABI create(const clap_plugin_factory_t*,const clap_host_t*,const char* id){
    if(std::strcmp(id,description.id)!=0)return nullptr;
    return new clap_plugin_t{&description,nullptr,init,destroy,activate,nothing,start,nothing,nothing,process,extension,nothing};
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
