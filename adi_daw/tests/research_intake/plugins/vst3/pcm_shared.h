// Bounded research PCM capture. Original code MIT, 2026-11-17.
#pragma once
#include "../platform.h"
#include <stdint.h>
#include <cstring>
#include <cstddef>
constexpr uint64_t ADI_PCM_FRAMES=1u<<20;
constexpr uint64_t ADI_PCM_CHUNKS=4096;
constexpr size_t ADI_PCM_HEADER_BYTES=4096;
struct alignas(8) ADIPCMHeader{
 char magic[8];uint32_t version,chunk_size;
 uint64_t capacity_frames,capacity_chunks;
 volatile adi_platform::Atomic64 next_frame,next_chunk,lost_frames,lost_chunks;
 int64_t qpc_frequency;volatile adi_platform::Atomic32 enabled;uint32_t reserved;
};
struct alignas(8) ADIPCMChunk{
 volatile adi_platform::Atomic64 committed;
 int64_t qpc0,qpc1;uint64_t start_frame;double transport;
 uint32_t frames,flags,tid,cpu;int32_t mode,rt;
};
static_assert(sizeof(ADIPCMHeader)==80&&offsetof(ADIPCMHeader,enabled)==72);
static_assert(sizeof(ADIPCMChunk)==64&&offsetof(ADIPCMChunk,frames)==40);
constexpr size_t ADI_PCM_DATA_OFFSET=ADI_PCM_HEADER_BYTES+ADI_PCM_CHUNKS*sizeof(ADIPCMChunk);
constexpr size_t ADI_PCM_MAP_BYTES=ADI_PCM_DATA_OFFSET+ADI_PCM_FRAMES*sizeof(double);
struct ADIPCMTrace{
 adi_platform::Mapping mapping;ADIPCMHeader *h=nullptr;
 bool open(){if(h)return true;if(!mapping.openEnvironment("ADI_ENGINE_MAP",ADI_PCM_MAP_BYTES,"_PCM"))return false;h=static_cast<ADIPCMHeader*>(mapping.data());if(memcmp(h->magic,"ADIPCM01",8)||h->version!=1||h->chunk_size!=sizeof(ADIPCMChunk)||h->capacity_frames!=ADI_PCM_FRAMES||h->capacity_chunks!=ADI_PCM_CHUNKS){close();return false;}return true;}
 void close(){h=nullptr;mapping.close();}
 bool enabled()const{return h&&adi_platform::acquire(&h->enabled);}
 template<class T> void append(const T *samples,uint32_t frames,ADIPCMChunk chunk){
  if(!h||!samples||!frames)return;
  const auto c=adi_platform::fetchAdd(&h->next_chunk,1);
  if(c<0||static_cast<uint64_t>(c)>=ADI_PCM_CHUNKS){adi_platform::fetchAdd(&h->lost_chunks,1);adi_platform::fetchAdd(&h->lost_frames,frames);return;}
  const auto f=adi_platform::fetchAdd(&h->next_frame,frames);
  if(f<0||uint64_t(f)+frames>ADI_PCM_FRAMES){adi_platform::fetchAdd(&h->lost_chunks,1);adi_platform::fetchAdd(&h->lost_frames,frames);return;}
  double *dst=adi_platform::mappedAt<double,ADI_PCM_DATA_OFFSET>(h)+f;
  for(uint32_t i=0;i<frames;++i)dst[i]=(double)samples[i];
  chunk.committed=0;chunk.start_frame=static_cast<uint64_t>(f);chunk.frames=frames;chunk.tid=adi_platform::tid();chunk.cpu=adi_platform::cpu();
  ADIPCMChunk *out=adi_platform::mappedAt<ADIPCMChunk,ADI_PCM_HEADER_BYTES>(h)+c;memcpy(out,&chunk,sizeof(chunk));adi_platform::publish(&out->committed,c+1);
 }
};
