// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/plugin_scanner.hpp"
#include "adi/appdata.hpp"
#include "adi/audio/io_audit.hpp"
#include <nlohmann/json.hpp>
#include <utf8proc.h>
#include <array>
#include <chrono>
#include <set>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace adi::scan {
namespace {
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool text(std::string_view value, std::size_t maximum = 65536) {
    if (value.size()>maximum || value.find('\0')!=value.npos) return false;
    std::size_t offset=0;
    while(offset<value.size()) {
        utf8proc_int32_t code=0;
        const auto size=utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(value.data()+offset),
            static_cast<utf8proc_ssize_t>(value.size()-offset),&code);
        if(size<=0)return false;
        offset+=static_cast<std::size_t>(size);
    }
    return true;
}
std::string utf8(const std::filesystem::path& path) {
    const auto bytes=path.u8string();return {bytes.begin(),bytes.end()};
}
bool identity(const std::string& format,const std::string& path) {
    return (format=="clap" || format=="vst3") && !path.empty() && text(path) &&
        std::filesystem::path(std::u8string(path.begin(),path.end())).is_absolute();
}
void plugin(const Plugin& value,const std::string& format) {
    require(!value.id.empty() && text(value.id) && text(value.name) && text(value.vendor) &&
        text(value.version) && text(value.features) && text(value.data),"invalid scanner descriptor text");
    require(format=="clap" ? value.data.empty() : !value.data.empty(),"invalid format-specific scanner data");
}
void preflight(std::string_view packet) {
    require(!packet.empty() && packet.size()<=kMaximumPacket,"scanner packet byte bound");
    unsigned depth=0;std::size_t tokens=0, stringBytes=0;bool quoted=false,escaped=false;
    for(const char ch:packet) {
        if(quoted) {
            if(!escaped && ch=='"') {quoted=false;continue;}
            require(++stringBytes<=65536,"scanner string byte bound");
            if(escaped)escaped=false;else if(ch=='\\')escaped=true;
            continue;
        }
        if(ch=='"'){quoted=true;stringBytes=0;++tokens;}
        else if(ch=='{' || ch=='['){require(++depth<=8,"scanner nesting bound");++tokens;}
        else if(ch=='}' || ch==']'){require(depth>0,"scanner nesting mismatch");--depth;}
        else if(ch==',' || ch==':')++tokens;
        require(tokens<=4194304,"scanner token bound");
    }
    require(!quoted && depth==0,"incomplete scanner packet");
}
void fields(const Json& object,const std::set<std::string>& expected) {
    require(object.is_object() && object.size()==expected.size(),"scanner object fields");
    for(auto it=object.begin();it!=object.end();++it)require(expected.contains(it.key()),"unknown scanner field");
}
const std::set<std::string> rootFields{"schema","format","path","plugins"};
const std::set<std::string> pluginFields{"id","name","vendor","version","features","instrument","data"};
// nlohmann's DOM callback parser scans the growing parent array for discarded
// values after EVERY object: quadratic even when the callback keeps everything.
// A streaming duplicate-key pass plus the ordinary DOM parser remains linear
// in descriptors, without weakening duplicate-field rejection at large counts.
struct UniqueKeys final : nlohmann::json_sax<Json> {
    std::vector<std::set<std::string>> objects;
    bool null() override{return true;}
    bool boolean(bool) override{return true;}
    bool number_integer(Json::number_integer_t) override{return true;}
    bool number_unsigned(Json::number_unsigned_t) override{return true;}
    bool number_float(Json::number_float_t,const Json::string_t&) override{return true;}
    bool string(Json::string_t&) override{return true;}
    bool binary(Json::binary_t&) override{return false;}
    bool start_object(std::size_t) override{objects.emplace_back();return objects.size()<=2;}
    bool key(Json::string_t& value) override {
        if(objects.empty())return false;
        const auto& allowed=objects.size()==1?rootFields:pluginFields;
        return allowed.contains(value) && objects.back().insert(value).second;
    }
    bool end_object() override{if(objects.empty())return false;objects.pop_back();return true;}
    bool start_array(std::size_t) override{return true;}
    bool end_array() override{return true;}
    bool parse_error(std::size_t,const std::string&,const nlohmann::detail::exception&) override{return false;}
};
std::string string(const Json& object,const char* field) {
    require(object.at(field).is_string(),"scanner field is not text");return object.at(field).get<std::string>();
}
std::string failure(const ProcessResult& result) {
    switch(result.status) {
        case ProcessStatus::Crashed:return result.signal==0 ? "scanner child crashed" :
            "scanner child crashed (signal "+std::to_string(result.signal)+")";
        case ProcessStatus::TimedOut:return "scanner child timed out";
        case ProcessStatus::OutputLimit:return "scanner child exceeded output limit";
        case ProcessStatus::IoFailed:return "scanner child I/O failure";
        case ProcessStatus::Exited:return "scanner child exited "+std::to_string(result.exitCode);
        default:return "scanner child could not run";
    }
}
} // namespace

std::optional<std::vector<Plugin>> decodeMetadata(std::string_view packet,
    const std::string& format,const std::string& path,std::string& error) {
    if(audio::onAudioThread)return std::nullopt;
    error.clear();
    try {
        require(identity(format,path),"invalid scanner request identity");preflight(packet);
        UniqueKeys keys;require(Json::sax_parse(packet,&keys),"invalid/duplicate scanner fields");
        const auto document=Json::parse(packet);
        fields(document,rootFields);
        require(document.at("schema").is_number_integer() && document.at("schema")==1,"unsupported scanner schema");
        require(string(document,"format")==format && string(document,"path")==path,"scanner reply identity mismatch");
        const auto& rows=document.at("plugins");
        require(rows.is_array() && rows.size()<=kMaximumPlugins,"scanner descriptor count");
        std::vector<Plugin> result;result.reserve(rows.size());std::set<std::string> ids;
        for(const auto& row:rows) {
            fields(row,pluginFields);require(row.at("instrument").is_boolean(),"scanner instrument is not boolean");
            Plugin value;value.id=string(row,"id");value.name=string(row,"name");value.vendor=string(row,"vendor");
            value.version=string(row,"version");value.features=string(row,"features");value.data=string(row,"data");
            value.instrument=row.at("instrument").get<bool>();plugin(value,format);
            require(ids.insert(value.id).second,"duplicate scanner descriptor identity");result.push_back(std::move(value));
        }
        return result;
    } catch(const std::exception&) {error="invalid or unsupported scanner metadata";return std::nullopt;}
}

std::optional<std::string> encodeMetadata(const std::string& format,const std::string& path,
    const std::vector<Plugin>& plugins,std::string& error) {
    if(audio::onAudioThread)return std::nullopt;
    error.clear();
    try {
        require(identity(format,path) && plugins.size()<=kMaximumPlugins,"invalid scanner metadata identity/count");
        Json rows=Json::array();std::set<std::string> ids;std::size_t bytes=path.size();
        for(const auto& value:plugins) {
            plugin(value,format);require(ids.insert(value.id).second,"duplicate scanner descriptor identity");
            const auto added=value.id.size()+value.name.size()+value.vendor.size()+value.version.size()+value.features.size()+value.data.size();
            require(added<=kMaximumPacket-bytes,"scanner metadata byte bound");bytes+=added;
            rows.push_back({{"id",value.id},{"name",value.name},{"vendor",value.vendor},{"version",value.version},
                {"features",value.features},{"instrument",value.instrument},{"data",value.data}});
        }
        auto result=Json{{"schema",1},{"format",format},{"path",path},{"plugins",std::move(rows)}}.dump();
        // Escapes/field overhead count in the actual packet, not just authored
        // text. Keep encoder and decoder bounds identical before child output.
        preflight(result);return result;
    } catch(const std::exception&) {error="scanner metadata exceeds its format or bounds";return std::nullopt;}
}

std::filesystem::path defaultScannerExecutable() {
    if(audio::onAudioThread)return {};
    std::filesystem::path executable;
#if defined(_WIN32)
    std::array<wchar_t,32768> buffer{};
    const auto size=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(size==0 || size>=buffer.size())return {};
    executable=std::filesystem::path(std::wstring(buffer.data(),size));
#elif defined(__APPLE__)
    std::uint32_t size=0;_NSGetExecutablePath(nullptr,&size);
    if(size==0 || size>65536)return {};
    std::vector<char> buffer(size);
    if(_NSGetExecutablePath(buffer.data(),&size)!=0)return {};
    executable=std::filesystem::absolute(buffer.data());
#else
    std::array<char,65536> buffer{};
    const auto size=readlink("/proc/self/exe",buffer.data(),buffer.size());
    if(size<=0 || static_cast<std::size_t>(size)>=buffer.size())return {};
    executable=std::filesystem::path(std::string(buffer.data(),static_cast<std::size_t>(size)));
#endif
#if defined(_WIN32)
    return executable.parent_path()/L"adi_plugin_scanner.exe";
#else
    return executable.parent_path()/"adi_plugin_scanner";
#endif
}
std::int64_t systemUtcMicros() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
Scanner::Scanner(Config config,std::unique_ptr<PluginRegistry> registry)
    :config_(std::move(config)),registry_(std::move(registry)){}
std::unique_ptr<Scanner> Scanner::open(const Config& config,std::string& error) {
    if(audio::onAudioThread)return nullptr;
    error.clear();
    try {
        Config resolved=config;
        if(resolved.executable.empty())resolved.executable=defaultScannerExecutable();
        if(resolved.registry.empty())resolved.registry=appdata::pluginRegistryFile();
        require(resolved.executable.is_absolute() && resolved.registry.is_absolute() &&
            resolved.clock!=nullptr,"scanner helper/registry/clock unavailable");
        const auto& process=resolved.process;
        require(process.timeout.count()>0 && process.timeout.count()<=300000 && process.maximumOutput>0 &&
            process.maximumOutput<=kMaximumPacket,"invalid scanner process bounds");
        auto registry=PluginRegistry::open(resolved.registry,error);if(!registry)return nullptr;
        return std::unique_ptr<Scanner>(new Scanner(std::move(resolved),std::move(registry)));
    } catch(const std::exception&) {error="scanner configuration unavailable";return nullptr;}
}
Result Scanner::probe(const std::string& format,const std::filesystem::path& path,Validator validate,void* context) {
    Result result;
    if(audio::onAudioThread){result.state=State::CallbackRefused;return result;}
    try {
        if(config_.process.cancel && config_.process.cancel->load(std::memory_order_relaxed)) {result.state=State::Cancelled;return result;}
        if(format!="clap" && (format!="vst3" || validate==nullptr)) {
            result.error="unsupported scanner format/validator";return result;
        }
        std::error_code ec;const auto canonical=std::filesystem::canonical(path,ec);
        if(ec){result.state=State::Unavailable;result.error="scan candidate path unavailable";return result;}
        result.path=utf8(canonical);require(identity(format,result.path),"invalid canonical scan identity");
        auto ticket=registry_->beginScan(format,result.path,config_.clock(),result.error);
        if(!ticket){result.state=State::Quarantined;return result;}
        const auto process=runProcess(config_.executable,{"--scan",format,result.path},config_.process);
        const bool cancel=process.succeeded() && config_.process.cancel && config_.process.cancel->load(std::memory_order_relaxed);
        const bool abandoned=cancel || process.status==ProcessStatus::Cancelled || process.status==ProcessStatus::LaunchFailed ||
            process.status==ProcessStatus::Invalid || process.status==ProcessStatus::CallbackRefused ||
            (process.status==ProcessStatus::Exited && process.exitCode==78);
        if(abandoned) {
            const auto state=cancel || process.status==ProcessStatus::Cancelled?State::Cancelled:State::Unavailable;
            std::string error;
            if(!registry_->finishScan(*ticket,ScanDisposition::Abandoned,"",config_.clock(),error)) {
                result.state=State::Quarantined;result.error=error;return result;
            }
            result.state=state;result.error="scanner helper unavailable or scan cancelled";return result;
        }
        std::optional<std::vector<Plugin>> plugins;
        if(process.succeeded())plugins=decodeMetadata(process.output,format,result.path,result.error);
        else result.error=failure(process);
        if(plugins && validate && !validate(*plugins,result.path,context,result.error))plugins.reset();
        if(plugins) {
            if(config_.process.cancel && config_.process.cancel->load(std::memory_order_relaxed)) {
                result.state=registry_->finishScan(*ticket,ScanDisposition::Abandoned,"",config_.clock(),result.error)?
                    State::Cancelled:State::Quarantined;return result;
            }
            if(!registry_->finishScan(*ticket,ScanDisposition::Succeeded,"",config_.clock(),result.error)) {
                result.state=State::Quarantined;return result;
            }
            result.state=State::Ready;result.plugins=std::move(*plugins);return result;
        }
        if(result.error.empty())result.error="invalid scanner format data";
        // Only bounded known diagnostics enter durable shared data. A validator
        // may return arbitrary diagnostic text; use a stable fallback if needed.
        if(!text(result.error,4096))result.error="scanner format validation failed";
        std::string finishError;
        if(!registry_->finishScan(*ticket,ScanDisposition::Failed,result.error,config_.clock(),finishError)) {
            result.state=State::Quarantined;result.error=finishError;return result;
        }
        result.state=State::Failed;return result;
    } catch(const std::exception&) {result.state=State::Unavailable;result.error="scan service exception; pending quarantine retained";return result;}
}
} // namespace adi::scan
