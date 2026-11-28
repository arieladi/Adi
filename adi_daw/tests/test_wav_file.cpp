// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/dsp/spectrum.hpp"
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
#include <functional>
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
 Bytes legacy(602,0);std::copy(m.description.begin(),m.description.end(),legacy.begin());
 std::copy(m.originator.begin(),m.originator.end(),legacy.begin()+256);set(legacy,338,m.timeReference,8);
 check(std::equal(legacy.begin(),legacy.end(),b.begin()+static_cast<std::ptrdiff_t>(ext+8)) && ext==96 && xml==706 && chunk(b,"data")==720,
       "legacy four-field metadata keeps every v0 bext byte, reserved byte and chunk offset");
 check(le(b,xml+4,4)==m.ixml.size() && std::memcmp(b.data()+xml+8,m.ixml.data(),m.ixml.size())==0 && b[xml+8+m.ixml.size()]==0,"iXML odd padding");
 WavReader r(p);check(r.frames()==1 && r.channels()==6,"metadata skips to data");
 auto bad=b;set(bad,chunk(bad,"fmt ")+36,1,1);refuses(p,bad,"invalid extensible GUID refused");
}
void metadataV2(const std::filesystem::path& p) {
 const std::array<std::uint16_t,5> codes{0xf727,0x04fd,0xff8f,0xd8f1,0x270f};
 for(const auto threshold:{std::uint64_t{1}<<32,std::uint64_t{1}})for(const auto* history:{"A=PCM\r\n","A=PCM,T=xx\r\n"}) {
  WavMetadata m{std::string(256,'d'),std::string(32,'o'),0x123456789abcdef0ULL,"<x/> "};
  m.bwfV2.emplace();auto& v=*m.bwfV2;
  v.originatorReference=std::string(32,'r');v.originationDate="2024-02-29";v.originationTime="23:59:58";
  for(std::size_t i=0;i<v.umid.size();++i)v.umid[i]=static_cast<std::uint8_t>(i);
  v.codingHistory=history;v.loudnessValue=-22.645;v.loudnessRange=12.765;v.maxTruePeakLevel=-1.125;
  v.maxMomentaryLoudness=-99.99;v.maxShortTermLoudness=99.99;
  Bytes expected(602,0);
  std::fill_n(expected.begin(),256,static_cast<unsigned char>('d'));std::fill_n(expected.begin()+256,32,static_cast<unsigned char>('o'));
  std::fill_n(expected.begin()+288,32,static_cast<unsigned char>('r'));
  std::copy(v.originationDate.begin(),v.originationDate.end(),expected.begin()+320);
  std::copy(v.originationTime.begin(),v.originationTime.end(),expected.begin()+330);
  set(expected,338,m.timeReference,8);set(expected,346,2,2);std::copy(v.umid.begin(),v.umid.end(),expected.begin()+348);
  for(std::size_t i=0;i<codes.size();++i)set(expected,412+i*2,codes[i],2);
  expected.insert(expected.end(),v.codingHistory.begin(),v.codingHistory.end());
  const std::array<float,3> input{-0.5f,0.25f,0.0f};
  {WavWriter w(p,48000,1,WavFormat::Pcm24,&m,threshold);m.description="changed after construction";v.loudnessValue=0;w.write(input.data(),3);w.close();}
  const auto bytes=load(p);const auto ext=chunk(bytes,"bext"),xml=chunk(bytes,"iXML"),data=chunk(bytes,"data");
  check(le(bytes,ext+4,4)==expected.size() && std::equal(expected.begin(),expected.end(),bytes.begin()+static_cast<std::ptrdiff_t>(ext+8)),
        "v2 complete fixed prefix/history encodes supplied provenance, high/low time, UMID, signed loudness and reserved zeros");
  check(xml==ext+8+expected.size()+(expected.size()&1U) && (!(expected.size()&1U) || bytes[xml-1]==0) && data==xml+14,
        "odd/even BWF history size excludes its RIFF pad and retains following odd iXML/data alignment");
  check(id(bytes,0,threshold==1?"RF64":"RIFF") && (threshold!=1 || le(bytes,20,8)==bytes.size()-8),"BWF variable chunk layout participates in RIFF/RF64 final sizing");
  WavReader reader(p);std::array<float,3> output{};
  check(reader.frames()==3 && reader.read(output.data(),3)==3 && input==output,"BWF v2 metadata leaves actual audio samples/length unchanged");
 }
 WavMetadata m{"unknown","adi",0,{}};m.bwfV2.emplace();
 auto fields=std::array{&m.bwfV2->loudnessValue,&m.bwfV2->loudnessRange,&m.bwfV2->maxTruePeakLevel,
                        &m.bwfV2->maxMomentaryLoudness,&m.bwfV2->maxShortTermLoudness};
 for(std::size_t chosen=0;chosen<=fields.size();++chosen) {
  for(auto* field:fields)field->reset();
  if(chosen<fields.size())*fields[chosen]=chosen==1?0.125:-0.125;
  {WavWriter writer(p,48000,1,WavFormat::Pcm16,&m);writer.close();}
  const auto bytes=load(p);const auto ext=chunk(bytes,"bext");bool exact=true;
  for(std::size_t i=0;i<fields.size();++i)exact&=le(bytes,ext+8+412+2*i,2)==(i==chosen?(chosen==1?13U:0xfff3U):0x7fffU);
  check(exact && le(bytes,ext+4,4)==602 && le(bytes,ext+8+346,2)==2,"each missing loudness field independently encodes 0x7fff, while signed half ties round away from zero");
  check(std::all_of(bytes.begin()+static_cast<std::ptrdiff_t>(ext+8+288),bytes.begin()+static_cast<std::ptrdiff_t>(ext+8+338),[](auto x){return x==0;}),
        "absent provenance date/time/reference stays zero-filled without inventing a clock or identifier");
 }
 // The older path deliberately keeps its original truncation/raw-byte behavior.
 WavMetadata old{std::string(300,'x'),std::string(40,'y'),7,{}};old.description[1]=static_cast<char>(0xff);
 {WavWriter writer(p,48000,1,WavFormat::Pcm16,&old);writer.close();}
 const auto bytes=load(p);const auto ext=chunk(bytes,"bext");
 check(le(bytes,ext+4,4)==602 && le(bytes,ext+8+346,2)==0 && bytes[ext+9]==0xff && bytes[ext+8+255]=='x' && bytes[ext+8+287]=='y',
       "opting out of v2 preserves legacy long/raw text acceptance and v0 layout");
}
void invalidMetadataV2(const std::filesystem::path& p) {
 using Edit=std::function<void(WavMetadata&)>;
 const std::vector<Edit> edits{
  [](auto& m){m.description=std::string(257,'x');},[](auto& m){m.originator=std::string(33,'x');},
  [](auto& m){m.bwfV2->originatorReference=std::string(33,'x');},[](auto& m){m.description=std::string("a\0b",3);},
  [](auto& m){m.originator="caf\xc3\xa9";},[](auto& m){m.bwfV2->originationDate="2023-02-29";},
  [](auto& m){m.bwfV2->originationDate="2024-04-31";},[](auto& m){m.bwfV2->originationDate="2024-13-01";},
  [](auto& m){m.bwfV2->originationDate="2024-01-00";},[](auto& m){m.bwfV2->originationDate="2024-1-01";},
  [](auto& m){m.bwfV2->originationDate="202x-01-01";},[](auto& m){m.bwfV2->originationTime="24:00:00";},
  [](auto& m){m.bwfV2->originationTime="12:60:00";},[](auto& m){m.bwfV2->originationTime="12:00:60";},
  [](auto& m){m.bwfV2->originationTime="1:00:00";},[](auto& m){m.bwfV2->originationTime="1x:00:00";},
  [](auto& m){m.bwfV2->codingHistory="row\n";},[](auto& m){m.bwfV2->codingHistory="row\r";},
  [](auto& m){m.bwfV2->codingHistory="row\r\nnext";},[](auto& m){m.bwfV2->codingHistory=std::string("a\0b\r\n",5);},
  [](auto& m){m.bwfV2->codingHistory="caf\xc3\xa9\r\n";},[](auto& m){m.bwfV2->loudnessRange=-0.01;}};
 const Bytes original{'k','e','e','p'};
 const auto rejected=[&](const WavMetadata& metadata) {
  save(p,original);bool refused=false;
  try{WavWriter writer(p,48000,1,WavFormat::Pcm16,&metadata);}catch(const std::invalid_argument&){refused=true;}
  check(refused && load(p)==original,"invalid v2 metadata is refused before opening or truncating an existing output");
 };
 for(const auto& edit:edits){WavMetadata m;m.bwfV2.emplace();edit(m);rejected(m);}
 for(std::size_t field=0;field<5;++field)for(const auto value:{100.0,-100.0,std::numeric_limits<double>::infinity(),
       -std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
  WavMetadata m;m.bwfV2.emplace();auto& v=*m.bwfV2;
  const auto fields=std::array{&v.loudnessValue,&v.loudnessRange,&v.maxTruePeakLevel,&v.maxMomentaryLoudness,&v.maxShortTermLoudness};
  *fields[field]=value;rejected(m);
 }
 const auto missing=p.parent_path()/"refused.wav";WavMetadata bad;bad.bwfV2.emplace();bad.bwfV2->loudnessValue=100.0;
 bool refused=false;try{WavWriter writer(missing,48000,1,WavFormat::Pcm16,&bad);}catch(const std::invalid_argument&){refused=true;}
 check(refused && !std::filesystem::exists(missing),"invalid metadata never creates an absent output");
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
std::array<double,2> ditherBandEnergy(std::uint32_t rate,WavDither mode) {
    constexpr std::int32_t size=8192;
    const auto frequencyStep=static_cast<double>(rate)/size;
    adi::dsp::Spectrum spectrum(size,adi::dsp::Window::Hann);
    std::vector<float> error(size);
    std::vector<float> bins(static_cast<std::size_t>(spectrum.bins()));
    std::array<double,2> energy{};
    PcmQuantizer quantizer(rate,1,16,mode,6678);
    for(unsigned window=0;window<8;++window) {
        for(float& residual:error)residual=static_cast<float>(quantizer.quantize(0));
        spectrum.analyse(error,bins);
        for(std::size_t bin=1;bin<bins.size();++bin) {
            const double hz=static_cast<double>(bin)*frequencyStep;
            const double magnitude=bins[bin];
            if(hz>=1000 && hz<=5000)energy[0]+=magnitude*magnitude;
            if(hz>=15000 && hz<static_cast<double>(rate)/2)energy[1]+=magnitude*magnitude;
        }
    }
    return energy;
}
void shapedDither(const std::filesystem::path& file) {
    const auto modes={WavDither::Shaped1,WavDither::Shaped2,WavDither::Shaped3};
    for(const auto depth:{16U,24U}) {
        const auto format=depth==16?WavFormat::Pcm16:WavFormat::Pcm24;
        for(const auto mode:modes) {
            constexpr std::uint32_t frames=32768;
            std::vector<double> input(static_cast<std::size_t>(frames)*2);
            for(std::uint32_t i=0;i<frames;++i) {
                input[2*i]=i<1024?0.0:(i%2==0?1.0:-1.0);
                input[2*i+1]=i<frames/2?1.0:-1.0;
            }
            PcmQuantizer reference(44100,2,depth,mode,42);
            for(const auto value:input) (void)reference.quantize(value);
            check(reference.gain()>0 && reference.gain()<1 && reference.saturationCount()==0,
                  "shaped full-scale DC and alternating stereo peaks do not saturate");
            const auto write=[&](std::uint64_t seed,bool split) {
                WavWriter out(file,44100,2,format,nullptr,std::uint64_t{1}<<32,mode,seed);
                if(split) {
                    out.write(input.data(),127);
                    out.write(input.data()+254,frames-127);
                } else out.write(input.data(),frames);
                out.close();
            };
            write(42,false);
            const auto whole=load(file);
            write(42,true);
            check(whole==load(file),"shaped WAV is byte-identical across output partitions");
            write(43,true);
            check(whole!=load(file),"shaped WAV seed controls the dither stream");
        }
    }
    for(auto rate:{44100U,48000U}) {
        const auto triangular=ditherBandEnergy(rate,WavDither::Triangular);
        for(const auto selected:modes) {
            const auto effective=effectiveDither(selected,rate);
            if(effective==WavDither::Triangular)continue;
            const auto shaped=ditherBandEnergy(rate,selected);
            std::printf("Shaped dither %s %u Hz: 1-5 kHz %.3f dB, above 15 kHz %.3f dB relative to TPDF\n",
                ditherName(selected),rate,10*std::log10(shaped[0]/triangular[0]),
                10*std::log10(shaped[1]/triangular[1]));
            check(shaped[0]<triangular[0] && shaped[1]>triangular[1],
                  "SoX shaping lowers the 1-5 kHz band and raises energy above 15 kHz");
        }
    }
    for(const auto selected:modes) {
        PcmQuantizer requested(96000,1,24,selected,123),triangular(96000,1,24,WavDither::Triangular,123);
        bool exact=effectiveDither(selected,96000)==WavDither::Triangular && requested.gain()==1;
        for(unsigned i=0;i<2048;++i) {
            const double value=i==11?std::numeric_limits<double>::quiet_NaN():0.43125;
            if(requested.quantize(value)!=triangular.quantize(value))exact=false;
        }
        check(exact,"unsupported rate uses the exact seeded triangular stream, including after a NaN");
    }
    const std::array<float,3> input{-0.0f,std::numeric_limits<float>::quiet_NaN(),1.0f};
    {
        WavWriter out(file,44100,1,WavFormat::Float32,nullptr,std::uint64_t{1}<<32,WavDither::Shaped3,45);
        out.write(input.data(),3);
        check(out.quantizationGain()==1,"Float32 bypasses shaped gain and RNG");
    }
    WavReader reader(file);
    std::array<float,3> output{};
    check(reader.read(output.data(),3)==3 &&
          std::bit_cast<std::array<std::uint32_t,3>>(input)==std::bit_cast<std::array<std::uint32_t,3>>(output),
          "requested shaped Float32 export preserves all source bits");
}
void speakerMask(const std::filesystem::path& path) {
    constexpr auto threshold = std::uint64_t{1} << 32;
    constexpr std::uint32_t front = 0x0003; // FL, FR
    constexpr std::uint32_t surround = 0x060f; // FL, FR, FC, LFE, SL, SR
    const std::array<float, 6> channels{0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f};
    {
        WavWriter output(path, 48000, 6, WavFormat::Pcm24, nullptr, threshold,
                         WavDither::None, 0, surround);
        output.write(channels.data(), 1);
        output.close();
    }
    const auto surroundFile = load(path);
    const auto fmt = chunk(surroundFile, "fmt ");
    check(le(surroundFile, fmt + 8, 2) == 0xfffe && le(surroundFile, fmt + 28, 4) == surround,
          "explicit surround positions are stored in the extensible fmt chunk");
    {
        WavReader input(path);
        std::array<float, 6> readBack{};
        check(input.read(readBack.data(), 1) == 1 && readBack == channels,
              "setting speaker positions preserves the original interleaved samples");
    }
    {
        WavWriter output(path, 48000, 2, WavFormat::Float32, nullptr, threshold,
                         WavDither::None, 0, front);
        output.write(channels.data(), 1);
        output.close();
    }
    const auto stereo = load(path);
    const auto stereoFmt = chunk(stereo, "fmt ");
    check(le(stereo, stereoFmt + 8, 2) == 0xfffe && le(stereo, stereoFmt + 28, 4) == front,
          "explicit stereo also writes the extensible mask");
    for (const auto mask : {std::uint32_t{1}, std::uint32_t{0x80000001}}) {
        bool rejected = false;
        try {
            WavWriter invalid(path, 48000, 2, WavFormat::Pcm16, nullptr, threshold,
                              WavDither::None, 0, mask);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected && load(path) == stereo, "invalid mask refuses before truncating the destination");
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
 try {adi::test::TempDirectory tmp("wav_file","suite");auto p=tmp.path()/"test.wav";roundTrips(p);auto b=promotion(p);corrupt(p,b);shortSizes(p,b);metadata(p);metadataV2(p);invalidMetadataV2(p);doubleQuantization(p);dither(p);shapedDither(p);speakerMask(p);big(p);}
 catch(const std::exception& e){check(false,e.what());}
 std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
