// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/scala_import.hpp"
#include "adi/audio/io_audit.hpp"
#include <charconv>
#include <cmath>
#include <stdexcept>
namespace adi::engine {
namespace {
constexpr std::size_t byteLimit=8*1024*1024,lineLimit=65536,degreeLimit=65536;
constexpr std::uint64_t integerLimit=9007199254740991ULL;
bool space(char c) noexcept{return c==' ' || c=='\t' || c=='\v' || c=='\f';}
std::string_view trim(std::string_view line) noexcept {
    while(!line.empty() && space(line.front()))line.remove_prefix(1);
    while(!line.empty() && space(line.back()))line.remove_suffix(1);
    return line;
}
std::string_view token(std::string_view line) noexcept {
    line=trim(line);std::size_t count=0;
    while(count<line.size() && !space(line[count]) && line[count]!='!')++count;
    return line.substr(0,count);
}
struct Lines {
    std::string_view remaining;std::size_t number=0;
    std::optional<std::string_view> next() {
        if(remaining.empty())return {};
        const auto end=remaining.find_first_of("\r\n");auto line=remaining.substr(0,end);++number;
        if(end==std::string_view::npos)remaining={};
        else {const auto separator=remaining[end];remaining.remove_prefix(end+1);if(separator=='\r' && !remaining.empty() && remaining.front()=='\n')remaining.remove_prefix(1);}
        if(line.size()>lineLimit)throw std::runtime_error("Scala physical line exceeds 65536 bytes");
        return line;
    }
    std::optional<std::string_view> content() {
        while(const auto line=next()){const auto value=trim(*line);if(!value.empty() && value.front()!='!')return value;}
        return {};
    }
};
std::uint64_t positiveInteger(std::string_view text) {
    if(text.empty())throw std::runtime_error("missing positive integer");
    for(const auto value:text)if(value<'0' || value>'9')throw std::runtime_error("Scala ratio/count must be an unsigned decimal integer");
    std::uint64_t result=0;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
    if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || result==0 || result>integerLimit)
        throw std::runtime_error("Scala integer exceeds positive exact-double range");
    return result;
}
double interval(std::string_view value) {
    double cents=0;
    if(value.find('.')!=std::string_view::npos) {
        if(!value.empty() && value.front()=='+')value.remove_prefix(1);
        const auto parsed=std::from_chars(value.data(),value.data()+value.size(),cents,std::chars_format::general);
        if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())throw std::runtime_error("invalid decimal cent interval");
    } else {
        const auto slash=value.find('/');const auto numerator=positiveInteger(value.substr(0,slash));
        const auto denominator=slash==std::string_view::npos?std::uint64_t{1}:positiveInteger(value.substr(slash+1));
        const auto top=static_cast<double>(numerator),bottom=static_cast<double>(denominator);
        // log1p retains very close ratios whose division can round to 1. The
        // operands are exact integers on every supported double platform.
        cents=1200*std::log1p((top-bottom)/bottom)/std::log(2.0);
    }
    if(!std::isfinite(cents) || cents<=0)throw std::runtime_error("Scala intervals must rise above implicit unison");
    return cents;
}
} // namespace
ScalaImport parseScala(std::string_view bytes,std::int64_t id,std::string_view name) {
    ScalaImport result;
    if(audio::onAudioThread){result.error="callback";return result;}
    Lines lines{bytes};
    try {
        if(id<=0 || name.size()>lineLimit)throw std::runtime_error("invalid explicit tuning identity/name");
        if(bytes.size()>byteLimit || bytes.find('\0')!=std::string_view::npos)throw std::runtime_error("Scala input exceeds 8 MiB or contains NUL");
        if(bytes.starts_with("\xef\xbb\xbf"))lines.remaining.remove_prefix(3);
        std::optional<std::string_view> description;
        // The first non-comment physical line is the description, including
        // an empty description. Preserve its authored spacing and bytes.
        while(const auto line=lines.next()){const auto value=trim(*line);if(value.empty() || value.front()!='!'){description=*line;break;}}
        if(!description)throw std::runtime_error("missing Scala description");
        const auto countLine=lines.content();if(!countLine)throw std::runtime_error("missing Scala interval count");
        const auto count=positiveInteger(token(*countLine));if(count>degreeLimit)throw std::runtime_error("Scala degree count exceeds 65536");
        TuningDefinition definition;definition.id=id;definition.name=name;definition.source="scala";definition.description=*description;
        definition.degrees.reserve(static_cast<std::size_t>(count));definition.degrees.push_back({0,0,""});double previous=0;
        for(std::uint64_t degree=1;degree<=count;++degree) {
            const auto line=lines.content();if(!line)throw std::runtime_error("Scala input ends before its declared intervals");
            const auto cents=interval(token(*line));if(cents<=previous)throw std::runtime_error("Scala intervals must strictly rise");
            if(degree==count)definition.periodCents=cents;
            else definition.degrees.push_back({static_cast<std::int64_t>(degree),cents,""});
            previous=cents;
        }
        if(lines.content())throw std::runtime_error("Scala input has content after its declared intervals");
        ClipScale scale;scale.tuningId=id;std::string error;
        // Validate the same definition the editor and native writer accept.
        std::optional<TuningDefinition> prepared(std::move(definition));
        if(!compileScaleContext(scale,prepared,error))throw std::runtime_error(error);
        result.definition=std::move(prepared);
    }catch(const std::exception& exception){result.definition.reset();result.line=lines.number;result.error=exception.what();}
    return result;
}
} // namespace adi::engine
