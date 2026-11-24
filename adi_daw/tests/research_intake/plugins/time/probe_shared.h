// ADI research instrumentation, 2026-11-17. Original research code, MIT licensed.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <cmath>
#include <cstring>

// One bounded shared ring; no allocation, waiting or disk I/O in record().
constexpr uint64_t ADI_CAPACITY = 1u << 20;
struct alignas(8) ADIRecord {
  volatile LONG64 committed;
  int64_t qpc0, qpc1;
  double transport, heard, decode, rate;
  int64_t steady;
  uint32_t kind, id, pid, tid, cpu, frames;
  int32_t rt, mode, priority;
  uint32_t flags, events;
  double input_rms, output_rms, parameter, work;
};
struct alignas(8) ADIHeader {
  char magic[8];
  uint32_t version, record_size;
  uint64_t capacity;
  volatile LONG64 next, lost, epoch;
  int64_t qpc_frequency;
  volatile LONG enabled, phase;
  uint32_t main_pid, reserved;
  volatile LONG64 hw_sequence, last_hw_qpc;
  double last_hw_transport;
};
constexpr size_t ADI_HEADER_BYTES=4096;
constexpr size_t ADI_MAP_BYTES=ADI_HEADER_BYTES + ADI_CAPACITY*sizeof(ADIRecord);
struct ADITrace {
  HANDLE mapping=nullptr;
  ADIHeader *h=nullptr;
  bool open() {
    if(h) return true;
    wchar_t name[256];
    if(!GetEnvironmentVariableW(L"ADI_ENGINE_MAP",name,256)) return false;
    mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
    if(!mapping) return false;
    h=(ADIHeader*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,ADI_MAP_BYTES);
    if(!h) { CloseHandle(mapping); mapping=nullptr; return false; }
    return !memcmp(h->magic,"ADIENG01",8) && h->record_size==sizeof(ADIRecord);
  }
  void close() { if(h)UnmapViewOfFile(h);if(mapping)CloseHandle(mapping);h=nullptr;mapping=nullptr; }
  bool enabled(int bit=2) const { return h && (h->enabled & bit); }
  void record(ADIRecord &r) {
    if(!h) return;
    LONG64 n=InterlockedIncrement64(&h->next)-1;
    if(n >= (LONG64)h->capacity) { InterlockedIncrement64(&h->lost); return; }
    r.pid=GetCurrentProcessId(); r.tid=GetCurrentThreadId(); r.cpu=GetCurrentProcessorNumber();
    r.priority=GetThreadPriority(GetCurrentThread());
    ADIRecord *dst=(ADIRecord*)((char*)h+ADI_HEADER_BYTES)+n;
    r.committed=0;
    memcpy(dst,&r,sizeof(r));
    InterlockedExchange64(&dst->committed,n+1);
  }
};
inline int64_t adi_qpc(){ LARGE_INTEGER x;QueryPerformanceCounter(&x);return x.QuadPart; }
using ADIGetAPI=void*(*)(const char*);
inline ADIGetAPI adi_api(){return (ADIGetAPI)GetProcAddress(GetModuleHandleW(L"reaper_adi_engine.dll"),"ADI_GetReaperAPI");}
// Fixed dependent arithmetic workload, reported as iterations/sample. Not a representative production FX.
inline void adi_work(uint32_t frames,int iterations,double &state){
  double x=state;
  for(uint64_t i=0,n=uint64_t(frames)*uint64_t(iterations);i<n;i++) x=x*0.9999997+0.000000123;
  state=x;
}
