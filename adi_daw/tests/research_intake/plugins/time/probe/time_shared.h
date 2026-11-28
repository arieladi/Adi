// Original diagnostic code from the director's research. MIT.
#pragma once
#include "../../platform.h"
#include <cstdint>
#include <cstring>
constexpr uint64_t TIME_CAP=16384;
struct TimeMarker { int32_t offset,reserved; double value; };
struct TimeParameter { uint32_t id; int32_t offset; double value; };
struct alignas(8) TimeRecord {
 volatile adi_platform::Atomic64 committed;
 int64_t qpc,project_samples,continuous_samples,system_ns;
 double sample_rate,project_music,bar_music,cycle_start_music,cycle_end_music,tempo;
 double gain_before,gain_after;
 uint32_t pid,tid,cpu,id,variant,latency,frames,sample_size,mode,context_present,context_state;
 int32_t numerator,denominator;
 uint32_t input_count,output_count,param_count,input_total,output_total,param_total,context_bytes;
 unsigned char raw_context[256];
 TimeMarker input[32],output[32];
 TimeParameter parameters[64];
};
struct alignas(8) TimeHeader { char magic[8]; uint32_t version,record_size;uint64_t capacity;volatile adi_platform::Atomic64 next,lost;volatile adi_platform::Atomic32 enabled,reserved;int64_t frequency;};
constexpr size_t TIME_HEADER_BYTES=4096,TIME_MAP_BYTES=TIME_HEADER_BYTES+TIME_CAP*sizeof(TimeRecord);
static_assert(sizeof(TimeHeader)==56&&offsetof(TimeHeader,enabled)==40);
static_assert(sizeof(TimeRecord)==2488&&offsetof(TimeRecord,raw_context)==184&&offsetof(TimeRecord,input)==440);
struct TimeTrace {
 adi_platform::Mapping map;TimeHeader*h=nullptr;
 bool open(){if(h)return true;if(!map.openEnvironment("ADI_TIME_MAP",TIME_MAP_BYTES))return false;
 h=static_cast<TimeHeader*>(map.data());
 if(memcmp(h->magic,"ADITIME1",8)||h->version!=1||h->record_size!=sizeof(TimeRecord)||h->capacity!=TIME_CAP){close();return false;}return true;}
 void close(){h=nullptr;map.close();}
 bool enabled()const{return h&&adi_platform::acquire(&h->enabled);}
 void record(TimeRecord&r){if(!h)return;auto n=adi_platform::fetchAdd(&h->next,1);if(n<0||static_cast<uint64_t>(n)>=h->capacity){adi_platform::fetchAdd(&h->lost,1);return;}
 r.pid=adi_platform::pid();r.tid=adi_platform::tid();r.cpu=adi_platform::cpu();r.committed=0;
 auto*p=adi_platform::mappedAt<TimeRecord,TIME_HEADER_BYTES>(h)+n;memcpy(p,&r,sizeof(r));adi_platform::publish(&p->committed,n+1);}
};

