// Bounded research PCM capture. Original code MIT, 2026-11-17.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <cstring>
#include <cstddef>
constexpr uint64_t ADI_PCM_FRAMES=1u<<20;
constexpr uint64_t ADI_PCM_CHUNKS=4096;
constexpr size_t ADI_PCM_HEADER_BYTES=4096;
struct alignas(8) ADIPCMHeader{
 char magic[8];uint32_t version,chunk_size;
 uint64_t capacity_frames,capacity_chunks;
 volatile LONG64 next_frame,next_chunk,lost_frames,lost_chunks;
 int64_t qpc_frequency;volatile LONG enabled;uint32_t reserved;
};
struct alignas(8) ADIPCMChunk{
 volatile LONG64 committed;
 int64_t qpc0,qpc1;uint64_t start_frame;double transport;
 uint32_t frames,flags,tid,cpu;int32_t mode,rt;
};
static_assert(sizeof(ADIPCMHeader)==80&&offsetof(ADIPCMHeader,enabled)==72);
static_assert(sizeof(ADIPCMChunk)==64&&offsetof(ADIPCMChunk,frames)==40);
constexpr size_t ADI_PCM_DATA_OFFSET=ADI_PCM_HEADER_BYTES+ADI_PCM_CHUNKS*sizeof(ADIPCMChunk);
constexpr size_t ADI_PCM_MAP_BYTES=ADI_PCM_DATA_OFFSET+ADI_PCM_FRAMES*sizeof(double);
struct ADIPCMTrace{
 HANDLE mapping=nullptr;ADIPCMHeader *h=nullptr;
 bool open(){wchar_t name[256];DWORD n=GetEnvironmentVariableW(L"ADI_ENGINE_MAP",name,248);if(!n||n>=248)return false;wcscat_s(name,L"_PCM");mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);if(!mapping)return false;h=(ADIPCMHeader*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,ADI_PCM_MAP_BYTES);if(!h){CloseHandle(mapping);mapping=nullptr;return false;}if(memcmp(h->magic,"ADIPCM01",8)||h->version!=1||h->chunk_size!=sizeof(ADIPCMChunk)||h->capacity_frames!=ADI_PCM_FRAMES||h->capacity_chunks!=ADI_PCM_CHUNKS){close();return false;}return true;}
 void close(){if(h)UnmapViewOfFile(h);if(mapping)CloseHandle(mapping);h=nullptr;mapping=nullptr;}
 bool enabled()const{return h&&h->enabled;}
 template<class T> void append(const T *samples,uint32_t frames,ADIPCMChunk chunk){
  if(!h||!samples||!frames)return;
  LONG64 c=InterlockedIncrement64(&h->next_chunk)-1;
  if(c>=(LONG64)ADI_PCM_CHUNKS){InterlockedIncrement64(&h->lost_chunks);InterlockedExchangeAdd64(&h->lost_frames,frames);return;}
  LONG64 f=InterlockedExchangeAdd64(&h->next_frame,frames);
  if(f<0||uint64_t(f)+frames>ADI_PCM_FRAMES){InterlockedIncrement64(&h->lost_chunks);InterlockedExchangeAdd64(&h->lost_frames,frames);return;}
  double *dst=(double*)((char*)h+ADI_PCM_DATA_OFFSET)+f;
  for(uint32_t i=0;i<frames;++i)dst[i]=(double)samples[i];
  chunk.committed=0;chunk.start_frame=f;chunk.frames=frames;chunk.tid=GetCurrentThreadId();chunk.cpu=GetCurrentProcessorNumber();
  ADIPCMChunk *out=(ADIPCMChunk*)((char*)h+ADI_PCM_HEADER_BYTES)+c;memcpy(out,&chunk,sizeof(chunk));InterlockedExchange64(&out->committed,c+1);
 }
};
