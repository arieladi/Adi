// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace adi::test {
namespace compact_flac_detail {
using Bytes = std::vector<std::uint8_t>;

constexpr std::uint8_t crc8(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc=0;
    for(const auto byte:bytes) {
        crc^=byte;
        for(unsigned bit=0;bit<8;++bit)
            crc=((crc<<1U)^((crc&0x80U)!=0?0x07U:0U))&0xffU;
    }
    return static_cast<std::uint8_t>(crc);
}
constexpr std::uint16_t crc16(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc=0;
    for(const auto byte:bytes) {
        crc^=static_cast<std::uint32_t>(byte)<<8U;
        for(unsigned bit=0;bit<8;++bit)
            crc=((crc<<1U)^((crc&0x8000U)!=0?0x8005U:0U))&0xffffU;
    }
    return static_cast<std::uint16_t>(crc);
}
inline constexpr std::array<std::uint8_t,9> crcCheck{0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39};
static_assert(crc8(crcCheck)==0xf4);
static_assert(crc16(crcCheck)==0xfee8);

inline void bigEndian(Bytes& bytes,std::uint64_t value,unsigned count) {
    for(unsigned remaining=count;remaining>0;--remaining)
        bytes.push_back(static_cast<std::uint8_t>((value>>(8U*(remaining-1U)))&0xffU));
}

inline void frameNumber(Bytes& bytes,std::uint32_t number) {
    if(number<0x80U){bytes.push_back(static_cast<std::uint8_t>(number));return;}
    if(number>0x7fffffffU)throw std::runtime_error("compact FLAC frame number exceeds 31 bits");
    unsigned count=2;
    std::uint32_t limit=0x800U;
    while(number>=limit){++count;limit<<=5U;}
    std::array<std::uint8_t,6> encoded{};
    for(unsigned i=count;i>1;--i) {
        encoded[i-1U]=static_cast<std::uint8_t>(0x80U|(number&0x3fU));
        number>>=6U;
    }
    encoded[0]=static_cast<std::uint8_t>((0xffU<<(8U-count))|number);
    for(unsigned i=0;i<count;++i)bytes.push_back(encoded[i]);
}

inline void write(std::ofstream& out,const Bytes& bytes) {
    out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if(!out)throw std::runtime_error("cannot write compact FLAC fixture");
}
} // namespace compact_flac_detail

// Original fixture writer, following RFC 9639 sections 8.2 and 9:
// https://www.rfc-editor.org/rfc/rfc9639.html
// Mono 48 kHz/16-bit silence, fixed 65535-sample blocks, constant subframes.
// Zero MD5 and frame-size fields mean unknown. An override of zero leaves the
// total unknown; any other override differing from the return value deliberately
// makes the STREAMINFO total false, while frame contents/CRCs remain intact.
// The caller owns the output path. No encoder executable or PCM allocation.
inline std::uint64_t writeCompactFlac(const std::filesystem::path& path,unsigned blockCount=2,
                                     std::optional<std::uint64_t> declaredFrames={}) {
    using namespace compact_flac_detail;
    constexpr std::uint64_t blockFrames=65535;
    constexpr std::uint64_t totalLimit=std::uint64_t{1}<<36U;
    const auto actualFrames=static_cast<std::uint64_t>(blockCount)*blockFrames;
    const auto declared=declaredFrames.value_or(actualFrames);
    if(blockCount==0 || actualFrames>=totalLimit || declared>=totalLimit)
        throw std::runtime_error("compact FLAC fixture dimensions exceed STREAMINFO bounds");

    Bytes metadata{0x66,0x4c,0x61,0x43,0x80,0x00,0x00,0x22};
    bigEndian(metadata,blockFrames,2);bigEndian(metadata,blockFrames,2);
    bigEndian(metadata,0,3);bigEndian(metadata,0,3);
    bigEndian(metadata,(std::uint64_t{48000}<<44U)|(std::uint64_t{15}<<36U)|declared,8);
    metadata.insert(metadata.end(),16,std::uint8_t{0});
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    if(!out)throw std::runtime_error("cannot create compact FLAC fixture");
    write(out,metadata);
    for(unsigned index=0;index<blockCount;++index) {
        Bytes frame{0xff,0xf8,0x7a,0x08}; // Fixed blocks, explicit 48 kHz, mono/16-bit.
        frameNumber(frame,index);
        bigEndian(frame,blockFrames-1U,2);
        frame.push_back(crc8(frame));
        frame.push_back(0); // Constant subframe, no wasted bits.
        bigEndian(frame,0,2); // Its one signed 16-bit sample.
        const auto checksum=crc16(frame);
        bigEndian(frame,checksum,2);
        write(out,frame);
    }
    out.close();
    if(!out)throw std::runtime_error("cannot close compact FLAC fixture");
    return actualFrames;
}
} // namespace adi::test
