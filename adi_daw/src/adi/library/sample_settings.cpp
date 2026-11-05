// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/library/sample_settings.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/warp.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace adi::library {
namespace {
using Json=nlohmann::json;
constexpr std::int64_t largest=std::numeric_limits<std::int64_t>::max();
bool nonnegative(const std::optional<std::int64_t>& v){return !v || *v>=0;}
bool pair(const std::optional<std::int64_t>& first,const std::optional<std::int64_t>& last){return first.has_value()==last.has_value() && (!first || (*first>=0 && *last>*first));}
bool validClock(const std::array<std::optional<std::int64_t>,4>& v){return pair(v[0],v[1]) && pair(v[2],v[3]);}
bool bounded(double value,double maximum){return std::isfinite(value) && std::abs(value)<=maximum;}
Json optional(const std::optional<std::int64_t>& value){return value?Json(*value):Json(nullptr);}
Json optional(const std::optional<double>& value){return value?Json(*value):Json(nullptr);}
Json encodeClock(const std::array<std::optional<std::int64_t>,4>& value){Json out=Json::array();for(const auto& field:value)out.push_back(optional(field));return out;}
std::int64_t integer(const Json& value){
    if(!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>()>static_cast<std::uint64_t>(largest)))throw std::invalid_argument("sample setting must be an exact signed integer");
    return value.get<std::int64_t>();
}
std::optional<std::int64_t> optionalInteger(const Json& value){return value.is_null()?std::nullopt:std::optional(integer(value));}
double real(const Json& value){if(!value.is_number())throw std::invalid_argument("sample setting must be numeric");const auto out=value.get<double>();if(!std::isfinite(out))throw std::invalid_argument("sample setting must be finite");return out;}
std::optional<double> optionalReal(const Json& value){return value.is_null()?std::nullopt:std::optional(real(value));}
bool boolean(const Json& value){if(!value.is_boolean())throw std::invalid_argument("sample setting must be boolean");return value.get<bool>();}
std::array<std::optional<std::int64_t>,4> decodeClock(const Json& value){if(!value.is_array() || value.size()!=4)throw std::invalid_argument("sample fade clock must have four fields");std::array<std::optional<std::int64_t>,4> out;for(std::size_t i=0;i<4;++i)out[i]=optionalInteger(value[i]);return out;}
std::string hex(std::span<const std::byte> bytes){constexpr char digits[]="0123456789abcdef";std::string out(bytes.size()*2,'0');for(std::size_t i=0;i<bytes.size();++i){const auto v=std::to_integer<unsigned>(bytes[i]);out[2*i]=digits[v>>4];out[2*i+1]=digits[v&15];}return out;}
int nibble(char c){return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:-1;}
std::optional<std::vector<std::byte>> markers(const Json& value){
    if(value.is_null())return {};
    if(!value.is_string())throw std::invalid_argument("sample Warp map must be hex or null");
    const auto& text=value.get_ref<const std::string&>();
    if(text.size()>kMaxSampleWarpBytes*2 || text.size()%2)throw std::invalid_argument("sample Warp hex exceeds bounds");
    // Validate all text before allocating the decoded bytes.
    if(!std::all_of(text.begin(),text.end(),[](char c){return nibble(c)>=0;}))throw std::invalid_argument("invalid sample Warp hex");
    std::vector<std::byte> bytes(text.size()/2);for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<std::byte>(nibble(text[2*i])*16+nibble(text[2*i+1]));return bytes;
}
SampleSettingsResult failure(SampleSettingsProblem p,std::string error={}){return {p,std::move(error),{}};}
}
SampleSettingsProblem validateSampleSettings(const SampleSettings& s,std::string& error) {
    if(audio::onAudioThread)return SampleSettingsProblem::Realtime;
    try {
        if(!s.sampleRate || s.sampleRate>768000 || !s.channels || s.channels>64 || !s.frames || s.frames>static_cast<std::uint64_t>(largest) || s.sourceStart>s.frames || !s.sourceLength || s.sourceLength>s.frames-s.sourceStart)throw std::invalid_argument("invalid saved sample dimensions/window");
        if((s.warpMode!="none" && !audio::parseWarpMode(s.warpMode)) || (s.warpEnabled && s.warpMode=="none") || s.channelMode<0 || s.channelMode>3 || !bounded(s.transposeSemis,96) || !bounded(s.formantShift,96) || !bounded(s.gainDb,120))throw std::invalid_argument("invalid saved sample sound controls");
        if(s.contentOffsetTicks<0 || s.loopStartTicks<0 || !nonnegative(s.loopLengthTicks) || !nonnegative(s.endMarkerTicks) || s.fadeInTicks<0 || s.fadeOutTicks<0 || s.fadeInCurve<0 || s.fadeInCurve>5 || s.fadeOutCurve<0 || s.fadeOutCurve>5 || (s.fadeInTension && !bounded(*s.fadeInTension,1)) || (s.fadeOutTension && !bounded(*s.fadeOutTension,1)))throw std::invalid_argument("invalid saved sample musical controls");
        if(!nonnegative(s.contentOffsetNs) || !nonnegative(s.sourceEndNs) || !pair(s.loopBeginNs,s.loopEndNs) || !validClock(s.fadeClockNs) || !validClock(s.fadeClockTicks))throw std::invalid_argument("invalid saved sample source clocks");
        if(s.loopLengthTicks && *s.loopLengthTicks>largest-s.loopStartTicks)throw std::invalid_argument("saved sample loop overflows");
        if(s.loopEnabled && (s.warpEnabled || !s.loopBeginNs) && (!s.loopLengthTicks || !*s.loopLengthTicks))throw std::invalid_argument("saved sample loop has no extent");
        if(!s.loopEnabled && s.endMarkerTicks && *s.endMarkerTicks<=s.contentOffsetTicks)throw std::invalid_argument("saved sample End precedes its content offset");
        if(s.signatureNumerator.has_value()!=s.signatureDenominator.has_value() || (s.signatureNumerator && (*s.signatureNumerator<1 || *s.signatureNumerator>64 || *s.signatureDenominator<1 || *s.signatureDenominator>64 || ((*s.signatureDenominator & (*s.signatureDenominator-1))!=0))))throw std::invalid_argument("invalid saved sample time signature");
        if(s.warpMarkers && (s.warpMarkers->size()>kMaxSampleWarpBytes || !audio::readWarpMap(*s.warpMarkers,s.sourceLength)))throw std::invalid_argument("invalid saved sample Warp map");
        if(s.warpEnabled && !s.warpMarkers)throw std::invalid_argument("saved sample Warp needs authored markers");
        error.clear();return SampleSettingsProblem::None;
    }catch(const std::bad_alloc&){return SampleSettingsProblem::Resource;}
    catch(const std::exception& e){error=e.what();return SampleSettingsProblem::Invalid;}
}
Json encodeSampleSettings(const SampleSettings& s) {
    if(audio::onAudioThread)return {};
    std::string error;const auto valid=validateSampleSettings(s,error);if(valid==SampleSettingsProblem::Resource)throw std::bad_alloc();if(valid!=SampleSettingsProblem::None)throw std::invalid_argument(error.empty()?"invalid sample settings":error);
    return {{"version",1},{"sample_rate",s.sampleRate},{"channels",s.channels},{"frames",s.frames},{"source_start",s.sourceStart},{"source_length",s.sourceLength},
        {"warp_enabled",s.warpEnabled},{"warp_mode",s.warpMode},{"warp_markers",s.warpMarkers?Json(hex(*s.warpMarkers)):Json(nullptr)},
        {"transpose_semis",s.transposeSemis},{"formant_shift",s.formantShift},{"gain_db",s.gainDb},{"reverse",s.reverse},{"channel_mode",s.channelMode},
        {"loop_enabled",s.loopEnabled},{"muted",s.muted},{"content_offset_ticks",s.contentOffsetTicks},{"loop_start_ticks",s.loopStartTicks},{"loop_length_ticks",optional(s.loopLengthTicks)},{"end_marker_ticks",optional(s.endMarkerTicks)},
        {"fade_in_ticks",s.fadeInTicks},{"fade_out_ticks",s.fadeOutTicks},{"fade_in_curve",s.fadeInCurve},{"fade_out_curve",s.fadeOutCurve},{"fade_in_tension",optional(s.fadeInTension)},{"fade_out_tension",optional(s.fadeOutTension)},
        {"content_offset_ns",optional(s.contentOffsetNs)},{"loop_begin_ns",optional(s.loopBeginNs)},{"loop_end_ns",optional(s.loopEndNs)},{"source_end_ns",optional(s.sourceEndNs)},
        {"fade_clock_ns",encodeClock(s.fadeClockNs)},{"fade_clock_ticks",encodeClock(s.fadeClockTicks)},{"signature_numerator",optional(s.signatureNumerator)},{"signature_denominator",optional(s.signatureDenominator)}};
}
SampleSettingsResult decodeSampleSettings(const Json& json) {
    // Not failure(): its error argument is a second string, which MSVC's Debug CRT
    // gives its own iterator-proxy allocation and free on the audio thread.
    if(audio::onAudioThread)return {SampleSettingsProblem::Realtime,{},{}};
    try {
        if(!json.is_object() || !json.contains("version") || !json.at("version").is_number_integer())return failure(SampleSettingsProblem::Invalid,"invalid sample settings envelope");
        if(json.at("version")!=1)return failure(SampleSettingsProblem::Unsupported,"unsupported sample settings version");
        // Exact current shape. Unknown future fields are never silently lost.
        if(json.size()!=34)return failure(SampleSettingsProblem::Unsupported,"unsupported sample settings fields");
        SampleSettings s;
        const auto rate=integer(json.at("sample_rate")),channels=integer(json.at("channels")),frames=integer(json.at("frames")),first=integer(json.at("source_start")),length=integer(json.at("source_length"));
        if(rate<1 || rate>768000 || channels<1 || channels>64 || frames<1 || first<0 || length<1)return failure(SampleSettingsProblem::Invalid,"invalid saved sample dimensions");
        s.sampleRate=static_cast<std::uint32_t>(rate);s.channels=static_cast<std::uint32_t>(channels);s.frames=static_cast<std::uint64_t>(frames);s.sourceStart=static_cast<std::uint64_t>(first);s.sourceLength=static_cast<std::uint64_t>(length);
        s.warpEnabled=boolean(json.at("warp_enabled"));if(!json.at("warp_mode").is_string())throw std::invalid_argument("sample Warp mode must be text");if(json.at("warp_mode").get_ref<const std::string&>().size()>16)throw std::invalid_argument("sample Warp mode exceeds bounds");s.warpMode=json.at("warp_mode").get<std::string>();s.warpMarkers=markers(json.at("warp_markers"));
        s.transposeSemis=real(json.at("transpose_semis"));s.formantShift=real(json.at("formant_shift"));s.gainDb=real(json.at("gain_db"));s.reverse=boolean(json.at("reverse"));s.channelMode=integer(json.at("channel_mode"));
        s.loopEnabled=boolean(json.at("loop_enabled"));s.muted=boolean(json.at("muted"));s.contentOffsetTicks=integer(json.at("content_offset_ticks"));s.loopStartTicks=integer(json.at("loop_start_ticks"));s.loopLengthTicks=optionalInteger(json.at("loop_length_ticks"));s.endMarkerTicks=optionalInteger(json.at("end_marker_ticks"));
        s.fadeInTicks=integer(json.at("fade_in_ticks"));s.fadeOutTicks=integer(json.at("fade_out_ticks"));s.fadeInCurve=integer(json.at("fade_in_curve"));s.fadeOutCurve=integer(json.at("fade_out_curve"));s.fadeInTension=optionalReal(json.at("fade_in_tension"));s.fadeOutTension=optionalReal(json.at("fade_out_tension"));
        s.contentOffsetNs=optionalInteger(json.at("content_offset_ns"));s.loopBeginNs=optionalInteger(json.at("loop_begin_ns"));s.loopEndNs=optionalInteger(json.at("loop_end_ns"));s.sourceEndNs=optionalInteger(json.at("source_end_ns"));s.fadeClockNs=decodeClock(json.at("fade_clock_ns"));s.fadeClockTicks=decodeClock(json.at("fade_clock_ticks"));
        s.signatureNumerator=optionalInteger(json.at("signature_numerator"));s.signatureDenominator=optionalInteger(json.at("signature_denominator"));
        std::string error;const auto valid=validateSampleSettings(s,error);if(valid!=SampleSettingsProblem::None)return failure(valid,error);return {SampleSettingsProblem::None,{},std::move(s)};
    }catch(const std::bad_alloc&){return failure(SampleSettingsProblem::Resource);}
    catch(const std::exception& e){return failure(SampleSettingsProblem::Invalid,e.what());}
}
} // namespace adi::library
