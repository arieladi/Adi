// Original diagnostic code from the director's research. MIT.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
constexpr uint64_t TIME_CAP=16384;
struct TimeMarker { int32_t offset,reserved; double value; };
struct TimeParameter { uint32_t id; int32_t offset; double value; };
struct alignas(8) TimeRecord {
 volatile LONG64 committed;
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
struct alignas(8) TimeHeader { char magic[8]; uint32_t version,record_size;uint64_t capacity;volatile LONG64 next,lost;volatile LONG enabled,reserved;int64_t frequency;};
constexpr size_t TIME_HEADER_BYTES=4096,TIME_MAP_BYTES=TIME_HEADER_BYTES+TIME_CAP*sizeof(TimeRecord);
struct TimeTrace {
 HANDLE map=nullptr;TimeHeader*h=nullptr;
 bool open(){wchar_t name[256];if(!GetEnvironmentVariableW(L"ADI_TIME_MAP",name,256))return false;
 map=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);if(!map)return false;
 h=(TimeHeader*)MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,TIME_MAP_BYTES);
 return h&&!memcmp(h->magic,"ADITIME1",8)&&h->record_size==sizeof(TimeRecord);}
 void close(){if(h)UnmapViewOfFile(h);if(map)CloseHandle(map);h=nullptr;map=nullptr;}
 bool enabled()const{return h&&h->enabled;}
 void record(TimeRecord&r){auto n=InterlockedIncrement64(&h->next)-1;if(n>=static_cast<LONG64>(h->capacity)){InterlockedIncrement64(&h->lost);return;}
 r.pid=GetCurrentProcessId();r.tid=GetCurrentThreadId();r.cpu=GetCurrentProcessorNumber();r.committed=0;
 auto*p=(TimeRecord*)((char*)h+TIME_HEADER_BYTES)+n;memcpy(p,&r,sizeof(r));InterlockedExchange64(&p->committed,n+1);}
};

