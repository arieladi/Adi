// ADI research instrumentation, 2026-11-17. Original research code, MIT licensed.
#pragma once
#include "platform.h"
#include <stdint.h>
#include <cmath>
#include <cstring>

// One bounded shared ring; no allocation, waiting or disk I/O in record().
constexpr uint64_t ADI_CAPACITY = 1u << 20;
struct alignas(8) ADIRecord {
  volatile adi_platform::Atomic64 committed;
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
  volatile adi_platform::Atomic64 next, lost, epoch;
  int64_t qpc_frequency;
  volatile adi_platform::Atomic32 enabled, phase;
  uint32_t main_pid, reserved;
  volatile adi_platform::Atomic64 hw_sequence, last_hw_qpc;
  double last_hw_transport;
};
constexpr size_t ADI_HEADER_BYTES=4096;
constexpr size_t ADI_MAP_BYTES=ADI_HEADER_BYTES + ADI_CAPACITY*sizeof(ADIRecord);
static_assert(sizeof(ADIRecord)==144&&offsetof(ADIRecord,frames)==84&&offsetof(ADIRecord,input_rms)==112);
static_assert(sizeof(ADIHeader)==96&&offsetof(ADIHeader,next)==24&&offsetof(ADIHeader,enabled)==56);
struct ADITrace {
  adi_platform::Mapping mapping;
  ADIHeader *h=nullptr;
  bool open() {
    if(h) return true;
    if(!mapping.openEnvironment("ADI_ENGINE_MAP",ADI_MAP_BYTES))return false;
    h=static_cast<ADIHeader*>(mapping.data());
    if(memcmp(h->magic,"ADIENG01",8)||h->record_size!=sizeof(ADIRecord)||h->capacity!=ADI_CAPACITY){close();return false;}
    return true;
  }
  void close() {h=nullptr;mapping.close();}
  bool enabled(int bit=2) const { return h && (adi_platform::acquire(&h->enabled) & bit); }
  void record(ADIRecord &r) {
    if(!h) return;
    const auto n=adi_platform::fetchAdd(&h->next,1);
    if(n<0||static_cast<uint64_t>(n)>=h->capacity) { adi_platform::fetchAdd(&h->lost,1); return; }
    r.pid=adi_platform::pid(); r.tid=adi_platform::tid(); r.cpu=adi_platform::cpu();
    r.priority=adi_platform::priority();
    ADIRecord *dst=adi_platform::mappedAt<ADIRecord,ADI_HEADER_BYTES>(h)+n;
    r.committed=0;
    memcpy(dst,&r,sizeof(r));
    adi_platform::publish(&dst->committed,n+1);
  }
};
inline int64_t adi_qpc(){return adi_platform::ticks();}
using ADIGetAPI=void*(*)(const char*);
inline ADIGetAPI adi_api(){return reinterpret_cast<ADIGetAPI>(adi_platform::optionalHostSymbol());}
// Fixed dependent arithmetic workload, reported as iterations/sample. Not a representative production FX.
inline void adi_work(uint32_t frames,int iterations,double &state){
  double x=state;
  for(uint64_t i=0,n=uint64_t(frames)*uint64_t(iterations);i<n;i++) x=x*0.9999997+0.000000123;
  state=x;
}
