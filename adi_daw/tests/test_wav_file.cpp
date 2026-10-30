// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "temp_directory.hpp"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>
using namespace adi::audio;
namespace {
// std::getenv, with MSVC's C4996 silenced at ONE site -- the same helper as
// clap_host.cpp's envOr. MSVC with /WX (tools/build.bat werror) refuses the
// bare call; no CI leg builds MSVC with /WX, so only a Windows build sees it.
const char* envVar(const char* name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
int checks=0, failures=0;
void check(bool ok,const char* name) { ++checks; if(!ok){++failures;std::printf("FAIL %s\n",name);} }
using Bytes=std::vector<unsigned char>;
Bytes load(const std::filesystem::path& p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
void save(const std::filesystem::path& p,const Bytes& b) {std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));}
std::uint64_t le(const Bytes& b,std::size_t at,unsigned n) {std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=static_cast<std::uint64_t>(b.at(at+i))<<(8*i);return v;}
void set(Bytes& b,std::size_t at,std::uint64_t v,unsigned n) {for(unsigned i=0;i<n;++i){b.at(at+i)=static_cast<unsigned char>(v&255U);v>>=8;}}
bool id(const Bytes& b,std::size_t at,const char* s) {return at+4<=b.size() && std::memcmp(b.data()+at,s,4)==0;}
std::size_t chunk(const Bytes& b,const char* s) { for(std::size_t at=12;at+8<=b.size();) {if(id(b,at,s))return at;const auto size=le(b,at+4,4);if(size>b.size()-at-8)break;at+=8+static_cast<std::size_t>(size+(size&1U));}throw std::runtime_error("chunk missing");}
void refuses(const std::filesystem::path& p,const Bytes& b,const char* label) {save(p,b);bool rejected=false;try {WavReader r(p);}catch(const std::exception&){rejected=true;}check(rejected,label);}
void roundTrips(const std::filesystem::path& p) {
 for(auto format:{WavFormat::Float32,WavFormat::Pcm24,WavFormat::Pcm16}) for(auto channels:{1,2,6,32}) {
  std::vector<float> input(static_cast<std::size_t>(channels)*129);
  for(std::size_t i=0;i<input.size();++i)input[i]=static_cast<float>(static_cast<int>(i%29)-14)/16;
  {WavWriter w(p,48000,static_cast<std::uint16_t>(channels),format);w.write(input.data(),65);w.write(input.data()+65*channels,64);w.close();w.close();}
  WavReader r(p);check(r.sampleRate()==48000 && r.channels()==channels && r.frames()==129 && r.format()==format,"roundtrip format and channel count");
  std::vector<float> output(input.size());check(r.read(output.data(),129)==129 && output==input,"roundtrip identical samples");
  check(r.read(output.data(),1)==0,"EOF");r.seek(64);check(r.read(output.data(),1)==1 && std::equal(output.begin(),output.begin()+channels,input.begin()+64*channels),"seek");
  const auto b=load(p);const auto fmt=chunk(b,"fmt ");const auto data=chunk(b,"data");
  check(id(b,0,"RIFF") && id(b,12,"JUNK") && le(b,16,4)==28 && le(b,4,4)==b.size()-8,"plain WAV and JUNK");
  check(le(b,fmt+8,2)==(channels>2?65534U:(format==WavFormat::Float32?3U:1U)),"format tag / EXTENSIBLE above stereo");
  check(le(b,data+4,4)==input.size()*(format==WavFormat::Pcm16?2U:format==WavFormat::Pcm24?3U:4U),"data byte size");
  if(channels>2)check(le(b,fmt+4,4)==40 && le(b,fmt+24,2)==22 && le(b,fmt+32,4)==(format==WavFormat::Float32?3U:1U),"extensible size and subformat");
 }
 // Odd PCM24 data length is padded, but the pad is not counted as a sample.
 {float x=-0.5f;WavWriter w(p,48000,1,WavFormat::Pcm24);w.write(&x,1);w.close();}
 auto b=load(p);auto d=chunk(b,"data");check(le(b,d+4,4)==3 && b.size()==d+12 && b.back()==0,"odd data padding");
 check(b[d+8]==0 && b[d+9]==0 && b[d+10]==0xc0,"PCM24 little endian known bytes");
 {float x=-0.5f;WavWriter w(p,48000,1,WavFormat::Float32);w.write(&x,1);w.close();}
 b=load(p);d=chunk(b,"data");check(le(b,d+8,4)==0xbf000000U && le(b,chunk(b,"fact")+8,4)==1,"float bytes and fact");
}
Bytes promotion(const std::filesystem::path& p) {
 constexpr std::uint32_t frames=262144; std::vector<float> in(frames+16,0.25f),out(in.size());
 {WavWriter w(p,48000,1,WavFormat::Float32,nullptr,1U<<20);w.write(in.data(),frames-1);
  auto before=load(p);check(id(before,0,"RIFF"),"below 1MiB stays WAV");
  w.write(in.data()+frames-1,17);auto after=load(p);
  check(id(after,0,"RF64") && id(after,12,"ds64"),"promotion visible before close");
  check(chunk(before,"data")==chunk(after,"data"),"promotion keeps data offset");w.close();}
 auto b=load(p);auto data=chunk(b,"data");
 check(id(b,0,"RF64") && le(b,4,4)==0xffffffffU && le(b,data+4,4)==0xffffffffU,"RF64 sentinels");
 check(le(b,20,8)==b.size()-8 && le(b,28,8)==in.size()*4 && le(b,36,8)==in.size() && le(b,44,4)==0,"ds64 exact sizes and frames");
 WavReader r(p);check(r.frames()==in.size() && r.read(out.data(),frames+16)==frames+16 && out==in,"promotion samples identical via ds64");return b;
}
void corrupt(const std::filesystem::path& p,const Bytes& good) {
 for(auto length:{0U,4U,11U,19U,47U,60U})refuses(p,Bytes(good.begin(),good.begin()+length),"truncated header refused");
 auto b=good;b.pop_back();refuses(p,b,"truncated data refused");
 b=good;b[0]='X';refuses(p,b,"bad magic refused");
 b=good;set(b,20,good.size(),8);refuses(p,b,"wrong ds64 RIFF size refused");
 b=good;set(b,28,good.size(),8);refuses(p,b,"wrong ds64 data size refused");
 b=good;set(b,36,7,8);refuses(p,b,"wrong RF64 sample count refused");
 b=good;b[12]='X';refuses(p,b,"missing ds64 refused");
 b=good;set(b,44,1,4);refuses(p,b,"truncated ds64 table refused");
 b=good;set(b,chunk(b,"fmt ")+8,99,2);refuses(p,b,"unsupported format refused");
 b=good;set(b,chunk(b,"fmt ")+20,0,2);refuses(p,b,"invalid alignment refused");
 b=good;set(b,chunk(b,"fmt ")+4,8,4);refuses(p,b,"short fmt refused");
 b=good;std::memcpy(b.data(),"BW64",4);set(b,36,0,8);save(p,b);
 {WavReader r(p);check(r.frames()==le(b,28,8)/4,"BW64 zero reserved sample count");}
 set(b,36,999,8);save(p,b);{WavReader r(p);check(r.frames()==le(b,28,8)/4,"BW64 ignores reserved words");}
}
void shortSizes(const std::filesystem::path& p,const Bytes& good) {
 for(const char* kind:{"RF64","BW64"}) {
  auto b=good;std::memcpy(b.data(),kind,4);set(b,4,b.size()-8,4);set(b,20,0,8);
  set(b,chunk(b,"data")+4,le(b,28,8),4);set(b,28,0,8);save(p,b);
  bool ok=false;try{WavReader r(p);ok=r.frames()==262160;}catch(const std::exception&){}
  check(ok,"non-sentinel 32-bit sizes take precedence over ds64");
 }
}
void metadata(const std::filesystem::path& p) {
 WavMetadata m{"test description","adi",0x123456789abcdef0ULL,"<x/> "};
 {WavWriter w(p,48000,6,WavFormat::Pcm24,&m);float f[6]{};w.write(f,1);w.close();}
 auto b=load(p);auto ext=chunk(b,"bext"),xml=chunk(b,"iXML");
 check(le(b,ext+4,4)==602 && le(b,ext+8+338,8)==m.timeReference,"bext size and time reference");
 check(std::memcmp(b.data()+ext+8,m.description.data(),m.description.size())==0 && std::memcmp(b.data()+ext+264,m.originator.data(),m.originator.size())==0,"bext text fields");
 check(le(b,xml+4,4)==m.ixml.size() && std::memcmp(b.data()+xml+8,m.ixml.data(),m.ixml.size())==0 && b[xml+8+m.ixml.size()]==0,"iXML odd padding");
 WavReader r(p);check(r.frames()==1 && r.channels()==6,"metadata skips to data");
 auto bad=b;set(bad,chunk(bad,"fmt ")+36,1,1);refuses(p,bad,"invalid extensible GUID refused");
}
void doubleQuantization(const std::filesystem::path& p) {
 for(const auto format:{WavFormat::Pcm16,WavFormat::Pcm24}) {
  const double scale=format==WavFormat::Pcm16?32768.0:8388608.0;
  const double half=0.5/scale;
  const std::vector<double> input{half-0x1p-52,half,half+0x1p-52,
      -half+0x1p-52,-half,-half-0x1p-52,1,-1,1e300,-1e300,
      std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()};
  const std::vector<double> codes{0,1,1,0,-1,-1,scale-1,-scale,scale-1,-scale,0,0};
  {WavWriter w(p,48000,1,format);w.write(input.data(),static_cast<std::uint32_t>(input.size()));}
  WavReader r(p);std::vector<float> out(input.size());
  check(r.read(out.data(),static_cast<std::uint32_t>(out.size()))==out.size(),"double PCM length");
  bool exact=true;for(std::size_t i=0;i<out.size();++i)exact=exact && out[i]*scale==codes[i];
  check(exact,"double PCM rounds sub-float half-LSB boundaries, signed ties and finite extremes correctly");
 }
 const std::array<std::uint32_t,4> patterns{0x80000000U,0x7fc12345U,0x3f800000U,0xff800000U};
 std::array<float,4> samples{};for(std::size_t i=0;i<samples.size();++i)samples[i]=std::bit_cast<float>(patterns[i]);
 {WavWriter w(p,48000,1,WavFormat::Float32,nullptr,std::uint64_t{1}<<32,WavDither::Triangular);w.write(samples.data(),4);}
 auto b=load(p);auto d=chunk(b,"data");bool exact=true;
 for(std::size_t i=0;i<patterns.size();++i)exact=exact && le(b,d+8+4*i,4)==patterns[i];
 check(exact,"float input preserves NaN payload, signed zero and infinity bits; float output never dithers");
 bool rejected=false;
 try {WavWriter w(p,48000,1,WavFormat::Float32);const double large=1e300;w.write(&large,1);}catch(const std::exception&){rejected=true;}
 check(rejected,"finite double beyond float range is refused for Float32");
 rejected=false;
 try {WavWriter w(p,48000,1,WavFormat::Pcm16,nullptr,std::uint64_t{1}<<32,static_cast<WavDither>(99));}catch(const std::exception&){rejected=true;}
 check(rejected,"unknown WAV dither is refused");
}
void dither(const std::filesystem::path& p) {
 constexpr std::uint32_t frames=65536;
 std::vector<double> silence(frames*2,0.0);
 for(const auto format:{WavFormat::Pcm16,WavFormat::Pcm24}) {
  const double scale=format==WavFormat::Pcm16?32768.0:8388608.0;
  auto write=[&](std::uint64_t seed,bool split) {
   WavWriter w(p,48000,2,format,nullptr,std::uint64_t{1}<<32,WavDither::Triangular,seed);
   if(split) {w.write(silence.data(),13);w.write(silence.data()+26,4097);
    w.write(static_cast<const double*>(nullptr),0);w.write(silence.data()+8220,frames-4110);}
   else w.write(silence.data(),frames);
   w.close();
  };
  write(42,false);const auto whole=load(p);
  std::vector<float> out(silence.size());{WavReader r(p);(void)r.read(out.data(),frames);}
  bool bounded=true,separate=false;double sum=0,squares=0,cross=0;
  for(std::size_t i=0;i<out.size();++i) {const double code=out[i]*scale;
   bounded=bounded && (code==-1 || code==0 || code==1);sum+=code;squares+=code*code;
   if(i%2==0){separate=separate || out[i]!=out[i+1];cross+=code*out[i+1]*scale;}
  }
  check(bounded && std::abs(sum/static_cast<double>(out.size()))<0.01 &&
      squares/static_cast<double>(out.size())>0.24 && squares/static_cast<double>(out.size())<0.26,
      "TPDF silence has bounded one-LSB codes, zero mean and quarter-LSB squared variance");
  check(separate && std::abs(cross/frames)<0.01,"stereo dither is not correlated or duplicated between channels");
  write(42,true);check(load(p)==whole,"TPDF stream is identical across write and internal buffer boundaries");
  write(43,false);check(load(p)!=whole,"different seeds produce different dither streams");
  {WavWriter w(p,48000,2,format);w.write(silence.data(),frames);}
  {WavReader r(p);(void)r.read(out.data(),frames);}
  check(std::all_of(out.begin(),out.end(),[](float v){return v==0;}),"legacy PCM writer default keeps digital silence");
  std::vector<double> quarter(frames,0.25/scale);
  {WavWriter w(p,48000,1,format,nullptr,std::uint64_t{1}<<32,WavDither::Rectangular,42);w.write(quarter.data(),frames);}
  {WavReader r(p);(void)r.read(out.data(),frames);}
  sum=0;bounded=true;for(std::uint32_t i=0;i<frames;++i){const double v=out[i]*scale;sum+=v;bounded=bounded && (v==0 || v==1);}
  check(bounded && std::abs(sum/frames-0.25)<0.01,"rectangular half-LSB dither removes quarter-LSB DC rounding bias");
 }
}
void big(const std::filesystem::path& p) {
 if(!envVar("ADI_WAV_BIG") || std::strcmp(envVar("ADI_WAV_BIG"),"1")!=0){std::puts("SKIP real >4GiB test (set ADI_WAV_BIG=1)");return;}
 constexpr std::uint32_t block=262144; constexpr std::uint64_t frames=(std::uint64_t{1}<<30)+block;
 std::vector<float> buf(block,0.25f);{WavWriter w(p,48000,1,WavFormat::Float32);for(std::uint64_t n=0;n<frames;n+=block)w.write(buf.data(),block);w.close();}
 check(std::filesystem::file_size(p)>(std::uint64_t{1}<<32),"real file exceeds 4GiB");
 WavReader r(p);check(r.frames()==frames,"real ds64 frame count");r.seek(frames-block);std::vector<float> out(block);check(r.read(out.data(),block)==block && out==buf,"real >4GiB tail seek/read");
}
}
int main() {
 try {adi::test::TempDirectory tmp("wav_file","suite");auto p=tmp.path()/"test.wav";roundTrips(p);auto b=promotion(p);corrupt(p,b);shortSizes(p,b);metadata(p);doubleQuantization(p);dither(p);big(p);}
 catch(const std::exception& e){check(false,e.what());}
 std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
