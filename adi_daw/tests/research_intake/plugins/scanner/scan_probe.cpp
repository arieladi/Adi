// Original ADI research diagnostic, MIT, 2026-11-18.
// VST3 interface definitions: Steinberg pluginterfaces 4f547e8e (MIT).
// Metadata-only fixture: intentionally has no working audio component.
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include "pluginterfaces/base/ipluginbase.h"
using namespace Steinberg;
static void log_event(const char* kind){
  char path[32768]{};if(!GetEnvironmentVariableA("ADI_SCAN_LOG",path,sizeof(path)))return;
  FILE* f=nullptr;fopen_s(&f,path,"a");if(f){fprintf(f,"%lu,%s\n",GetCurrentProcessId(),kind);fclose(f);}
}
static int option(const char* key){char text[32]{};GetEnvironmentVariableA(key,text,sizeof(text));return atoi(text);}
static bool eq(const char* a,const char* b){return a&&b&&!memcmp(a,b,16);}
static const TUID ids[2]={INLINE_UID(0x41444953,0x43414E50,0x524F4245,0x00000001),INLINE_UID(0x41444953,0x43414E50,0x524F4245,0x00000002)};
class Factory final:public IPluginFactory2 {
 std::atomic<uint32> refs{1};
 public:
 tresult PLUGIN_API queryInterface(const TUID iid,void** p) override{if(!p)return kInvalidArgument;*p=nullptr;if(eq(iid,FUnknown_iid)||eq(iid,IPluginFactory_iid)||eq(iid,IPluginFactory2_iid))*p=(IPluginFactory2*)this;if(!*p)return kNoInterface;addRef();return kResultOk;}
 uint32 PLUGIN_API addRef() override{return ++refs;}
 uint32 PLUGIN_API release() override{return --refs;}
 tresult PLUGIN_API getFactoryInfo(PFactoryInfo* p) override{log_event("getFactoryInfo");if(!p)return kInvalidArgument;*p={};strcpy_s(p->vendor,"ADI Scanner Research");strcpy_s(p->url,"https://example.invalid");return kResultOk;}
 int32 PLUGIN_API countClasses() override{log_event("countClasses");return option("ADI_SCAN_ONE_CLASS")?1:2;}
 tresult PLUGIN_API getClassInfo(int32 i,PClassInfo* p) override{log_event("getClassInfo");if(!p||i<0||i>1)return kInvalidArgument;*p={};memcpy(p->cid,ids[i],16);p->cardinality=PClassInfo::kManyInstances;strcpy_s(p->category,"Audio Module Class");strcpy_s(p->name,i?"ADI Scan Probe B":"ADI Scan Probe A");return kResultOk;}
 tresult PLUGIN_API getClassInfo2(int32 i,PClassInfo2* p) override{log_event("getClassInfo2");if(!p||i<0||i>1)return kInvalidArgument;*p={};memcpy(p->cid,ids[i],16);p->cardinality=PClassInfo::kManyInstances;strcpy_s(p->category,"Audio Module Class");strcpy_s(p->name,i?"ADI Scan Probe B":"ADI Scan Probe A");strcpy_s(p->vendor,"ADI Scanner Research");strcpy_s(p->version,option("ADI_SCAN_VERSION_TWO")?"2.0.0":"1.0.0");strcpy_s(p->sdkVersion,"VST 3.8.0");strcpy_s(p->subCategories,"Fx|Tools");return kResultOk;}
 tresult PLUGIN_API createInstance(FIDString,FIDString,void** p) override{log_event("createInstance");if(p)*p=nullptr;return kNoInterface;}
};
extern "C" __declspec(dllexport) IPluginFactory* PLUGIN_API GetPluginFactory(){
 log_event("GetPluginFactory");
 int mode=option("ADI_SCAN_FAIL");if(mode==1)return nullptr;if(mode==2)Sleep(15000);if(mode==3)TerminateProcess(GetCurrentProcess(),73);
 static Factory f;f.addRef();return &f;
}
extern "C" __declspec(dllexport) bool InitDll(){log_event("InitDll");return true;}
extern "C" __declspec(dllexport) bool ExitDll(){log_event("ExitDll");return true;}
