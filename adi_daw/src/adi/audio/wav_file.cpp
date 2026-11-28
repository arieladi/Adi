// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "wav_file.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace adi::audio {
namespace {
constexpr std::uint64_t sentinel = 0xffffffffULL;
constexpr std::array<unsigned char,12> guidTail{0,0,16,0,128,0,0,170,0,56,155,113};
static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
void require(bool ok) { if (!ok) throw std::runtime_error("invalid or unsupported WAV"); }
std::streamoff offset(std::uint64_t n) {
    require(n <= static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()));
    return static_cast<std::streamoff>(n);
}
void put(std::ostream& f, std::uint64_t v, unsigned n) {
    for(unsigned i=0;i<n;++i) { f.put(static_cast<char>(v & 255U)); v >>= 8; }
}
std::uint64_t get(std::istream& f, unsigned n) {
    std::uint64_t v=0;
    for(unsigned i=0;i<n;++i) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(f.get())) << (i*8);
    return v;
}
void tag(std::ostream& f, const char* s) { f.write(s,4); }
std::string tag(std::istream& f) { std::array<char,4> b{}; f.read(b.data(),4); return {b.data(),4}; }
void zeros(std::ostream& f, unsigned n) { for(unsigned i=0;i<n;++i) f.put(0); }
void textField(std::ostream& f, const std::string& s, unsigned n) {
    const auto count=std::min<std::size_t>(s.size(),n);
    f.write(s.data(),static_cast<std::streamsize>(count)); zeros(f,n-static_cast<unsigned>(count));
}
std::uint16_t sampleBytes(WavFormat f) {
    switch(f) { case WavFormat::Pcm16:return 2; case WavFormat::Pcm24:return 3; case WavFormat::Float32:return 4; }
    throw std::invalid_argument("WAV format");
}
void metadataRequire(bool ok,const char* field) {
    if(!ok)throw std::invalid_argument(field);
}
void asciiField(const std::string& value,std::size_t maximum,const char* field) {
    metadataRequire(value.size()<=maximum && std::all_of(value.begin(),value.end(),[](unsigned char c){return c!=0 && c<=127;}),field);
}
unsigned decimal(const std::string& value,std::size_t first,std::size_t count) {
    unsigned result=0;
    for(std::size_t i=first;i<first+count;++i) {
        metadataRequire(value[i]>='0' && value[i]<='9',"BWF date/time digits");
        result=result*10+static_cast<unsigned>(value[i]-'0');
    }
    return result;
}
std::uint16_t loudnessCode(const std::optional<double>& value,bool range) {
    if(!value)return 0x7fff;
    metadataRequire(std::isfinite(*value) && *value>=(range?0.0:-99.99) && *value<=99.99,"BWF loudness out of range");
    const auto code=static_cast<std::int16_t>(std::round(*value*100.0));
    return static_cast<std::uint16_t>(code);
}
std::array<std::uint16_t,5> validateV2(const WavMetadata& metadata) {
    const auto& v=*metadata.bwfV2;
    asciiField(metadata.description,256,"BWF description");
    asciiField(metadata.originator,32,"BWF originator");
    asciiField(v.originatorReference,32,"BWF originator reference");
    asciiField(v.originationDate,10,"BWF origination date");
    if(!v.originationDate.empty()) {
        metadataRequire(v.originationDate.size()==10,"BWF origination date length");
        const auto year=decimal(v.originationDate,0,4),month=decimal(v.originationDate,5,2),day=decimal(v.originationDate,8,2);
        constexpr std::array<unsigned,12> days{31,28,31,30,31,30,31,31,30,31,30,31};
        metadataRequire(month>=1 && month<=12,"BWF origination month");
        const bool leap=year%4==0 && (year%100!=0 || year%400==0);
        metadataRequire(day>=1 && day<=days[month-1]+static_cast<unsigned>(month==2 && leap),"BWF origination day");
    }
    asciiField(v.originationTime,8,"BWF origination time");
    if(!v.originationTime.empty()) {
        metadataRequire(v.originationTime.size()==8,"BWF origination time length");
        metadataRequire(decimal(v.originationTime,0,2)<=23 && decimal(v.originationTime,3,2)<=59 &&
                        decimal(v.originationTime,6,2)<=59,"BWF origination time range");
    }
    metadataRequire(v.codingHistory.size()<sentinel-602,"BWF coding history size");
    asciiField(v.codingHistory,static_cast<std::size_t>(sentinel-603),"BWF coding history ASCII");
    metadataRequire(v.codingHistory.empty() || v.codingHistory.ends_with("\r\n"),"BWF coding history termination");
    for(std::size_t i=0;i<v.codingHistory.size();++i) {
        if(v.codingHistory[i]=='\r') {
            metadataRequire(i+1<v.codingHistory.size() && v.codingHistory[i+1]=='\n',"BWF coding history CR/LF");
            ++i;
        }else metadataRequire(v.codingHistory[i]!='\n',"BWF coding history CR/LF");
    }
    return {loudnessCode(v.loudnessValue,false),loudnessCode(v.loudnessRange,true),loudnessCode(v.maxTruePeakLevel,false),
            loudnessCode(v.maxMomentaryLoudness,false),loudnessCode(v.maxShortTermLoudness,false)};
}
double pcmScale(WavFormat format) noexcept { return format == WavFormat::Pcm16 ? 32768.0 : 8388608.0; }
std::int32_t quantizePcm(double sample, double scale, double dither) noexcept {
    if (!std::isfinite(sample))
        return 0;
    // Clamp before scaling so even a finite double cannot overflow during scaling.
    return static_cast<std::int32_t>(std::clamp(std::round(std::clamp(sample, -2.0, 2.0) * scale + dither),
                                               -scale, scale - 1));
}
}
float wavSampleAtDepth(float value, WavFormat format) noexcept {
    if (format == WavFormat::Float32)
        return value;
    const auto scale = pcmScale(format);
    return static_cast<float>(quantizePcm(value, scale, 0.0) / scale);
}
bool validWavSpeakerMask(std::uint16_t channels, std::uint32_t mask) noexcept {
    constexpr std::uint32_t definedSpeakers = 0x0003ffff;
    return mask == 0 || ((mask & ~definedSpeakers) == 0 && std::popcount(mask) == channels);
}

WavWriter::WavWriter(const std::filesystem::path& path, std::uint32_t rate,
                     std::uint16_t channels, WavFormat format, const WavMetadata* metadata,
                     std::uint64_t threshold, WavDither dither, std::uint64_t seed, std::uint32_t speakerMask)
    : format_(format), channels_(channels), bytes_(sampleBytes(format)), threshold_(threshold) {
    require(validDither(dither));
    if (!validWavSpeakerMask(channels, speakerMask)) {
        throw std::invalid_argument("WAV speaker mask needs one defined speaker per interleaved channel");
    }
    const auto align=static_cast<std::uint32_t>(channels)*bytes_;
    require(channels && rate && align<=65535 && static_cast<std::uint64_t>(align)*rate<=sentinel);
    require(!metadata || metadata->ixml.size()<sentinel);
    std::array<std::uint16_t,5> loudness{};
    if(metadata && metadata->bwfV2)loudness=validateV2(*metadata);
    if(format!=WavFormat::Float32)quantizer_.emplace(rate,channels,bytes_*8U,dither,seed);
    file_.exceptions(std::ios::failbit|std::ios::badbit);
    file_.open(path,std::ios::binary|std::ios::in|std::ios::out|std::ios::trunc);
    tag(file_,"RIFF"); put(file_,0,4); tag(file_,"WAVE");
    tag(file_,"JUNK"); put(file_,28,4); zeros(file_,28);
    const bool extensible=channels>2 || speakerMask!=0;
    const unsigned fmtSize=extensible?40U:(format==WavFormat::Float32?18U:16U);
    tag(file_,"fmt "); put(file_,fmtSize,4);
    const unsigned encoding=format==WavFormat::Float32?3U:1U;
    put(file_,extensible?0xfffeU:encoding,2); put(file_,channels,2); put(file_,rate,4);
    put(file_,static_cast<std::uint64_t>(rate)*align,4); put(file_,align,2); put(file_,bytes_*8U,2);
    if(extensible) {
        put(file_,22,2); put(file_,bytes_*8U,2); put(file_,speakerMask,4);
        put(file_,encoding,4);
        for(auto c:guidTail) file_.put(static_cast<char>(c));
    } else if(format==WavFormat::Float32) put(file_,0,2); // WAVEFORMATEX cbSize
    if(format==WavFormat::Float32) {
        tag(file_,"fact"); put(file_,4,4); factOffset_=static_cast<std::uint64_t>(file_.tellp()); put(file_,0,4);
    }
    if(metadata) {
        const auto* v=metadata->bwfV2?&*metadata->bwfV2:nullptr;
        tag(file_,"bext"); put(file_,602+(v?v->codingHistory.size():0),4);
        textField(file_,metadata->description,256); textField(file_,metadata->originator,32);
        if(v) {
            textField(file_,v->originatorReference,32);textField(file_,v->originationDate,10);textField(file_,v->originationTime,8);
            put(file_,metadata->timeReference,8);put(file_,2,2);
            for(const auto byte:v->umid)file_.put(static_cast<char>(byte));
            for(const auto code:loudness)put(file_,code,2);
            zeros(file_,180);
            file_.write(v->codingHistory.data(),static_cast<std::streamsize>(v->codingHistory.size()));
            if(v->codingHistory.size()&1U)file_.put(0);
        }else {
            zeros(file_,32+10+8); put(file_,metadata->timeReference,8); put(file_,0,2); zeros(file_,254);
        }
        if(!metadata->ixml.empty()) {
            tag(file_,"iXML"); put(file_,metadata->ixml.size(),4);
            file_.write(metadata->ixml.data(),static_cast<std::streamsize>(metadata->ixml.size()));
            if(metadata->ixml.size()&1U) file_.put(0);
        }
    }
    tag(file_,"data"); put(file_,0,4); dataOffset_=static_cast<std::uint64_t>(file_.tellp()); header();
}
WavWriter::~WavWriter() { try { close(); } catch(...) {} }
void WavWriter::header() {
    const auto end=dataOffset_+dataBytes_;
    const auto riff=end+(dataBytes_&1U)-8;
    promoted_=promoted_ || dataBytes_>=threshold_ || riff>=sentinel;
    file_.seekp(offset(end)); if(dataBytes_&1U) file_.put(0);
    file_.seekp(0); tag(file_,promoted_?"RF64":"RIFF"); put(file_,promoted_?sentinel:riff,4);
    file_.seekp(12); tag(file_,promoted_?"ds64":"JUNK"); put(file_,28,4);
    if(promoted_) { put(file_,riff,8); put(file_,dataBytes_,8); put(file_,dataBytes_/(channels_*bytes_),8); put(file_,0,4); }
    file_.seekp(offset(dataOffset_-4)); put(file_,promoted_?sentinel:dataBytes_,4);
    if(factOffset_) { file_.seekp(offset(factOffset_)); put(file_,std::min(dataBytes_/(channels_*bytes_),sentinel),4); }
    file_.flush(); file_.seekp(offset(end));
}
void WavWriter::write(const float* input, std::uint32_t frames) { writeSamples(input, frames); }
void WavWriter::write(const double* input, std::uint32_t frames) { writeSamples(input, frames); }
template<class Sample> void WavWriter::writeSamples(const Sample* input, std::uint32_t frames) {
    require(file_.is_open() && (input || !frames));
    const auto samples=static_cast<std::uint64_t>(frames)*channels_;
    require(samples<=std::numeric_limits<std::size_t>::max());
    const auto count=samples*bytes_;
    require(count<=static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())-dataOffset_-dataBytes_-1);
    std::array<char,65536> buffer{};
    const std::size_t capacity=buffer.size()/bytes_;
    for(std::uint64_t pos=0;pos<samples;) {
        const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(capacity,samples-pos));
        for(std::size_t i=0;i<n;++i) {
            const auto f=input[static_cast<std::size_t>(pos)+i]; std::uint32_t bits;
            if(format_==WavFormat::Float32) {
                require(!std::isfinite(f) || std::abs(static_cast<double>(f)) <= std::numeric_limits<float>::max());
                bits=std::bit_cast<std::uint32_t>(static_cast<float>(f));
            }
            else bits=static_cast<std::uint32_t>(quantizer_->quantize(static_cast<double>(f)));
            for(unsigned j=0;j<bytes_;++j) buffer[i*bytes_+j]=static_cast<char>((bits>>(j*8))&255U);
        }
        file_.write(buffer.data(),static_cast<std::streamsize>(n*bytes_)); pos+=n;
    }
    dataBytes_+=count; header();
}
void WavWriter::close() { if(file_.is_open()) { header(); file_.close(); } }

WavReader::WavReader(const std::filesystem::path& path) {
    fileIoPoint();
    file_.exceptions(std::ios::failbit|std::ios::badbit); file_.open(path,std::ios::binary);
    file_.seekg(0,std::ios::end); const auto physical=static_cast<std::uint64_t>(file_.tellg());
    require(physical>=12); file_.seekg(0);
    const auto kind=tag(file_); const bool large=kind=="RF64"||kind=="BW64";
    require(large||kind=="RIFF"); const auto shortSize=get(file_,4); require(tag(file_)=="WAVE");
    std::uint64_t limit=physical, dataSize64=0, sampleCount64=0;
    struct ChunkSize { std::string id; std::uint64_t size; bool used=false; };
    std::vector<ChunkSize> table;
    std::uint64_t at=12;
    if(large) {
        require(physical-at>=36 && tag(file_)=="ds64"); const auto size=get(file_,4);
        require(size>=28 && size<=physical-20);
        const auto storedRiff=get(file_,8); const auto riff=shortSize==sentinel?storedRiff:shortSize; dataSize64=get(file_,8); sampleCount64=get(file_,8); const auto entries=get(file_,4);
        require(riff>=4 && riff<=physical-8 && entries<=(size-28)/12);
        limit=riff+8; require(size<=limit-20);
        // Bounds-check on disk; no allocation proportional to unknown chunk bodies.
        for(std::uint64_t i=0;i<entries;++i) { auto id=tag(file_); auto n=get(file_,8); table.push_back({id,n,false}); }
        at=20+size+(size&1U); require(at<=limit);
    } else { require(shortSize>=4 && shortSize<=physical-8); limit=shortSize+8; }
    bool fmt=false, data=false;
    std::uint64_t dataSize=0;
    while(at<limit) {
        require(limit-at>=8); file_.seekg(offset(at)); const auto id=tag(file_); auto size=get(file_,4); at+=8;
        if(size==sentinel) {
            require(large);
            if(id=="data") size=dataSize64;
            else {
                auto it=std::find_if(table.begin(),table.end(),[&](const auto& item){return !item.used && item.id==id;});
                require(it!=table.end()); size=it->size; it->used=true;
            }
        }
        require(size<=limit-at && (size&1U)<=limit-at-size);
        if(id=="fmt ") {
            require(!fmt && size>=16); const auto encoding=get(file_,2);
            channels_=static_cast<std::uint16_t>(get(file_,2)); rate_=static_cast<std::uint32_t>(get(file_,4));
            const auto byteRate=get(file_,4); align_=static_cast<std::uint16_t>(get(file_,2)); const auto bits=get(file_,2);
            auto actual=encoding;
            if(encoding==0xfffe) {
                require(size>=40); const auto extension=get(file_,2); require(extension>=22 && extension<=size-18);
                require(get(file_,2)==bits); (void)get(file_,4); actual=get(file_,4);
                for(auto expected:guidTail) require(get(file_,1)==expected);
            } else if(size>16) { require(size>=18); require(get(file_,2)<=size-18); }
            require((actual==1 && (bits==16||bits==24))||(actual==3 && bits==32));
            bytes_=static_cast<std::uint16_t>(bits/8);
            require(channels_ && rate_ && static_cast<std::uint32_t>(channels_)*bytes_==align_ && static_cast<std::uint64_t>(rate_)*align_==byteRate);
            format_=actual==3?WavFormat::Float32:(bits==16?WavFormat::Pcm16:WavFormat::Pcm24); fmt=true;
        } else if(id=="data") {
            require(!data && fmt); data=true; dataOffset_=at; dataSize=size;
        } else if(id=="ds64") require(false); // misplaced/duplicate size authority
        at+=size+(size&1U);
    }
    require(fmt && data && dataSize%align_==0); frames_=dataSize/align_;
    // BW64 reserves the RF64 sampleCount words and mandates ignoring them.
    require(kind!="RF64" || sampleCount64==0 || sampleCount64==frames_);
    seek(0);
}
WavReader::~WavReader() { fileIoPoint(); }

void WavReader::seek(std::uint64_t frame) {
    fileIoPoint();
    require(frame<=frames_); file_.seekg(offset(dataOffset_+frame*align_)); position_=frame;
}
std::uint32_t WavReader::read(float* output, std::uint32_t frames) {
    fileIoPoint();
    const auto n=static_cast<std::uint32_t>(std::min<std::uint64_t>(frames,frames_-position_)); require(output||!n);
    const auto samples=static_cast<std::uint64_t>(n)*channels_; require(samples<=std::numeric_limits<std::size_t>::max());
    std::array<unsigned char,65536> buffer{}; const auto capacity=buffer.size()/bytes_;
    for(std::uint64_t pos=0;pos<samples;) {
        const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(capacity,samples-pos));
        file_.read(reinterpret_cast<char*>(buffer.data()),static_cast<std::streamsize>(count*bytes_));
        for(std::size_t i=0;i<count;++i) {
            std::uint32_t bits=0; for(unsigned j=0;j<bytes_;++j) bits|=static_cast<std::uint32_t>(buffer[i*bytes_+j])<<(j*8);
            float value;
            if(format_==WavFormat::Float32) value=std::bit_cast<float>(bits);
            else {
                const auto sign=std::uint32_t{1}<<(bytes_*8-1);
                const auto signedValue=static_cast<std::int32_t>(bits & (sign-1))-static_cast<std::int32_t>(bits & sign);
                value=static_cast<float>(signedValue)/(bytes_==2?32768.0f:8388608.0f);
            }
            output[static_cast<std::size_t>(pos)+i]=value;
        }
        pos+=count;
    }
    position_+=n; return n;
}
} // namespace adi::audio
