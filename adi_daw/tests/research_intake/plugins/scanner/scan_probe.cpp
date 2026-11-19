// Original ADI research diagnostic, MIT, 2026-11-18.
// VST3 interface definitions: Steinberg pluginterfaces 4f547e8e (MIT).
// Metadata-only fixture: intentionally has no working audio component.
#define NOMINMAX
#include "../platform.h"
#include <cstdio>
#include <cstring>
#include <atomic>
#include "pluginterfaces/base/ipluginbase.h"
using namespace Steinberg;
static void log_event(const char* kind){
  const auto path=adi_platform::environment("ADI_SCAN_LOG");if(path.empty())return;
  FILE* f=adi_platform::appendFile(path.c_str());if(f){fprintf(f,"%u,%s\n",adi_platform::pid(),kind);fclose(f);}
}
static int option(const char* key){return std::atoi(adi_platform::environment(key).c_str());}
static bool eq(const char* a,const char* b){return a&&b&&!memcmp(a,b,16);}
static const TUID ids[2]={INLINE_UID(0x41444953,0x43414E50,0x524F4245,0x00000001),INLINE_UID(0x41444953,0x43414E50,0x524F4245,0x00000002)};
class Factory final:public IPluginFactory2 {
 std::atomic<uint32> refs{1};
 public:
 tresult PLUGIN_API queryInterface(const TUID requestedIid,void** p) override{if(!p)return kInvalidArgument;*p=nullptr;if(eq(requestedIid,FUnknown_iid)||eq(requestedIid,IPluginFactory_iid)||eq(requestedIid,IPluginFactory2_iid))*p=(IPluginFactory2*)this;if(!*p)return kNoInterface;addRef();return kResultOk;}
 uint32 PLUGIN_API addRef() override{return ++refs;}
 uint32 PLUGIN_API release() override{return --refs;}
 tresult PLUGIN_API getFactoryInfo(PFactoryInfo* p) override{log_event("getFactoryInfo");if(!p)return kInvalidArgument;*p={};adi_platform::copy(p->vendor,"ADI Scanner Research");adi_platform::copy(p->url,"https://example.invalid");return kResultOk;}
 int32 PLUGIN_API countClasses() override{log_event("countClasses");return option("ADI_SCAN_ONE_CLASS")?1:2;}
 tresult PLUGIN_API getClassInfo(int32 i,PClassInfo* p) override{log_event("getClassInfo");if(!p||i<0||i>1)return kInvalidArgument;*p={};memcpy(p->cid,ids[i],16);p->cardinality=PClassInfo::kManyInstances;adi_platform::copy(p->category,"Audio Module Class");adi_platform::copy(p->name,i?"ADI Scan Probe B":"ADI Scan Probe A");return kResultOk;}
 tresult PLUGIN_API getClassInfo2(int32 i,PClassInfo2* p) override{log_event("getClassInfo2");if(!p||i<0||i>1)return kInvalidArgument;*p={};memcpy(p->cid,ids[i],16);p->cardinality=PClassInfo::kManyInstances;adi_platform::copy(p->category,"Audio Module Class");adi_platform::copy(p->name,i?"ADI Scan Probe B":"ADI Scan Probe A");adi_platform::copy(p->vendor,"ADI Scanner Research");adi_platform::copy(p->version,option("ADI_SCAN_VERSION_TWO")?"2.0.0":"1.0.0");adi_platform::copy(p->sdkVersion,"VST 3.8.0");adi_platform::copy(p->subCategories,"Fx|Tools");return kResultOk;}
 tresult PLUGIN_API createInstance(FIDString,FIDString,void** p) override{log_event("createInstance");if(p)*p=nullptr;return kNoInterface;}
};
extern "C" ADI_EXPORT IPluginFactory* PLUGIN_API GetPluginFactory(){
 log_event("GetPluginFactory");
 int mode=option("ADI_SCAN_FAIL");if(mode==1)return nullptr;if(mode==2)adi_platform::sleepMilliseconds(15000);if(mode==3)adi_platform::exitChild(73);
 static Factory f;f.addRef();return &f;
}
#ifdef _WIN32
extern "C" ADI_EXPORT bool InitDll(){log_event("InitDll");return true;}
extern "C" ADI_EXPORT bool ExitDll(){log_event("ExitDll");return true;}

#elif defined(__APPLE__)
extern "C" ADI_EXPORT bool bundleEntry(CFBundleRef){return true;}
extern "C" ADI_EXPORT bool bundleExit(){return true;}
#else
extern "C" ADI_EXPORT bool ModuleEntry(void*){return true;}
extern "C" ADI_EXPORT bool ModuleExit(){return true;}
#endif
