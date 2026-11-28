// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/plugin_scanner.hpp"
#include "adi/scan_helper_protocol.hpp"
#include "adi/appdata.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/scan_stamp.hpp"
#include "adi/media/blake3.hpp"
#include "adi/windows_shortcut.hpp"
#include <algorithm>
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
constexpr auto helperCouldNotStart = "scanner helper could not start";
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
void plugin(const Plugin& value,const std::string& format,bool classIds) {
    require(!value.id.empty() && text(value.id) && text(value.name) && text(value.vendor) &&
        text(value.version) && text(value.features) && text(value.data),"invalid scanner descriptor text");
    require(format=="clap" ? value.data.empty() : !value.data.empty(),"invalid format-specific scanner data");
    if(!classIds) {
        require(value.classId.empty(),"legacy scanner descriptors cannot carry VST3 class IDs");
        return;
    }
    require(format=="vst3" && value.classId.size()==32 &&
        std::all_of(value.classId.begin(),value.classId.end(),[](char ch) {
            return (ch>='0' && ch<='9') || (ch>='a' && ch<='f');
        }),"invalid canonical VST3 class ID");
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
const std::set<std::string> vst3ClassFields{"id","name","vendor","version","features","instrument","data","class_id"};
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
        const auto& allowed=objects.size()==1?rootFields:vst3ClassFields;
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
        const auto& schema=document.at("schema");
        const bool classIds=schema==kVst3ClassIdPacketSchema && format=="vst3";
        require(schema.is_number_integer() && (schema==kPacketSchema || classIds),"unsupported scanner schema");
        require(string(document,"format")==format && string(document,"path")==path,"scanner reply identity mismatch");
        const auto& rows=document.at("plugins");
        require(rows.is_array() && rows.size()<=kMaximumPlugins,"scanner descriptor count");
        std::vector<Plugin> result;result.reserve(rows.size());std::set<std::string> ids,classes;
        for(const auto& row:rows) {
            fields(row,classIds?vst3ClassFields:pluginFields);
            require(row.at("instrument").is_boolean(),"scanner instrument is not boolean");
            Plugin value;value.id=string(row,"id");value.name=string(row,"name");value.vendor=string(row,"vendor");
            value.version=string(row,"version");value.features=string(row,"features");value.data=string(row,"data");
            value.instrument=row.at("instrument").get<bool>();
            if(classIds)value.classId=string(row,"class_id");
            plugin(value,format,classIds);
            if(classIds)require(classes.insert(value.classId).second,"duplicate VST3 class ID in module");
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
        const bool classIds=std::any_of(plugins.begin(),plugins.end(),[](const Plugin& value) {return !value.classId.empty();});
        Json rows=Json::array();std::set<std::string> ids,classes;std::size_t bytes=path.size();
        for(const auto& value:plugins) {
            plugin(value,format,classIds);
            require(ids.insert(value.id).second,"duplicate scanner descriptor identity");
            if(classIds)require(classes.insert(value.classId).second,"duplicate VST3 class ID in module");
            const auto added=value.id.size()+value.name.size()+value.vendor.size()+value.version.size()+value.features.size()+value.data.size()+value.classId.size();
            require(added<=kMaximumPacket-bytes,"scanner metadata byte bound");bytes+=added;
            rows.push_back({{"id",value.id},{"name",value.name},{"vendor",value.vendor},{"version",value.version},
                {"features",value.features},{"instrument",value.instrument},{"data",value.data}});
            if(classIds)rows.back()["class_id"]=value.classId;
        }
        auto result=Json{{"schema",classIds?kVst3ClassIdPacketSchema:kPacketSchema},{"format",format},{"path",path},{"plugins",std::move(rows)}}.dump();
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
std::vector<std::string> findCandidates(const std::vector<std::string>& roots,
                                        std::string_view extension, std::vector<Failure>* unreadable) {
    if (audio::onAudioThread) return {};
    namespace fs = std::filesystem;
    std::vector<fs::path> pending;
    for (const auto& bytes : roots)
        pending.emplace_back(std::u8string(bytes.begin(), bytes.end()));
    std::set<fs::path> visited, reported;
    std::set<std::string> candidates;
    const auto reportDenied = [&](const fs::path& path, const std::error_code& error) {
        if (unreadable && isAccessDenied(error) && reported.insert(path).second)
            unreadable->push_back({utf8(path), "Cannot read this search location: " + error.message(), State::UnreadablePath});
    };
    const auto suffixOf = [](const fs::path& path) {
        auto suffix = utf8(path.extension());
        for (char& c : suffix) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        return suffix;
    };
#if defined(_WIN32)
    const auto followShortcut = [&](fs::path path) -> std::optional<fs::path> {
        std::set<fs::path> links;
        for (unsigned hop = 0; hop <= 32; ++hop) {
            std::error_code error;
            path = fs::canonical(path, error);
            if (error) return std::nullopt;
            if (suffixOf(path) != ".lnk" || !fs::is_regular_file(path, error))
                return error ? std::nullopt : std::optional<fs::path>(path);
            if (hop == 32 || !links.insert(path).second) return std::nullopt;
            auto target = windowsShortcutTarget(path);
            if (!target) return std::nullopt;
            path = std::move(*target);
        }
        return std::nullopt;
    };
    const auto addShortcut = [&](const fs::path& link) {
        const auto target = followShortcut(link);
        if (!target) return;
        if (suffixOf(*target) == extension) candidates.insert(utf8(*target));
        else {
            std::error_code error;
            if (fs::is_directory(*target, error)) pending.push_back(*target);
        }
    };
#endif
    const fs::directory_iterator end;
    while (!pending.empty()) {
        auto path = std::move(pending.back());
        pending.pop_back();
        std::error_code ec;
        const auto dir = fs::canonical(path, ec);
        if (ec) {
            reportDenied(path, ec);
            continue;
        }
        if (!visited.insert(dir).second) continue;
#if defined(_WIN32)
        if (suffixOf(dir) == ".lnk" && fs::is_regular_file(dir, ec)) {
            addShortcut(dir);
            continue;
        }
        ec.clear();
#endif
        fs::directory_iterator it(dir, ec);
        for (; !ec && it != end; it.increment(ec)) {
            const auto suffix = suffixOf(it->path());
            std::error_code entryError;
            if (suffix == extension) {
                const auto module = fs::canonical(it->path(), entryError);
                if (!entryError) candidates.insert(utf8(module));
#if defined(_WIN32)
            } else if (suffix == ".lnk" && it->is_regular_file(entryError)) {
                addShortcut(it->path());
#endif
            } else if (it->is_directory(entryError)) {
                pending.push_back(it->path());
            }
            reportDenied(it->path(), entryError);
        }
        reportDenied(dir, ec);
    }
    return {candidates.begin(), candidates.end()};
}

std::int64_t systemUtcMicros() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
Scanner::Scanner(Config config,std::unique_ptr<PluginRegistry> registry,std::string buildHash,
    std::uintmax_t helperSize,std::filesystem::file_time_type helperTime)
    :config_(std::move(config)),registry_(std::move(registry)),helperBuildHash_(std::move(buildHash)),
     helperSize_(helperSize),helperTime_(helperTime){}
std::unique_ptr<Scanner> Scanner::open(const Config& config,std::string& error,State* reason) {
    if(reason)*reason=State::Unavailable;
    if(audio::onAudioThread) {
        if(reason)*reason=State::CallbackRefused;
        return nullptr;
    }
    error.clear();
    auto failureState=State::Unavailable;
    try {
        Config resolved=config;
        require(resolved.cacheMode==ScanCacheMode::Reuse || resolved.cacheMode==ScanCacheMode::Refresh ||
                resolved.cacheMode==ScanCacheMode::Clean,
                "invalid scanner cache mode");
        if(resolved.executable.empty())resolved.executable=defaultScannerExecutable();
        if(resolved.registry.empty())resolved.registry=appdata::pluginRegistryFile();
        require(resolved.executable.is_absolute() && resolved.registry.is_absolute() &&
            resolved.clock!=nullptr,"scanner helper/registry/clock unavailable");
        const auto& process=resolved.process;
        require(process.timeout.count()>0 && process.timeout.count()<=300000 && process.maximumOutput>0 &&
            process.maximumOutput<=kMaximumPacket,"invalid scanner process bounds");
        failureState=State::HelperCouldNotStart;
        const auto helperSize=std::filesystem::file_size(resolved.executable);
        const auto helperTime=std::filesystem::last_write_time(resolved.executable);
        const auto build=media::blake3File(resolved.executable);
        require(static_cast<bool>(build),"scanner helper build identity unavailable");
        require(helperSize==std::filesystem::file_size(resolved.executable) &&
                helperTime==std::filesystem::last_write_time(resolved.executable),"scanner helper changed during identification");
        failureState=State::Unavailable;
        auto registry=PluginRegistry::open(resolved.registry,error);if(!registry)return nullptr;
        return std::unique_ptr<Scanner>(new Scanner(std::move(resolved),std::move(registry),build.hex,helperSize,helperTime));
    } catch(const std::exception&) {
        if(reason)*reason=failureState;
        error=failureState==State::HelperCouldNotStart?helperCouldNotStart:"scanner configuration unavailable";
        return nullptr;
    }
}
std::unique_ptr<Scanner> Scanner::openPeer(std::string& error) const {
    if (audio::onAudioThread) return nullptr;
    auto registry = PluginRegistry::open(config_.registry, error);
    if (!registry) return nullptr;
    return std::unique_ptr<Scanner>(new Scanner(config_, std::move(registry), helperBuildHash_,
                                                helperSize_, helperTime_));
}

bool Scanner::retry(const std::string& format,const std::filesystem::path& path,std::string& error) {
    if(audio::onAudioThread)return false;
    error.clear();
    try {
        if(format!="clap" && format!="vst3"){error="unsupported scanner format";return false;}
        std::error_code ec;const auto canonical=std::filesystem::canonical(path,ec);
        if(ec){error="retry candidate path unavailable";return false;}
        return registry_->unblockScan(format,utf8(canonical),error);
    } catch(const std::exception&){error="scan retry unavailable";return false;}
}
Result Scanner::probe(const std::string& format,const std::filesystem::path& path,Validator validate,void* context) {
    Result result;
    if(audio::onAudioThread){result.state=State::CallbackRefused;return result;}
    try {
        if(config_.process.cancel && config_.process.cancel->load(std::memory_order_relaxed)) {result.state=State::Cancelled;return result;}
        if(format!="clap" && (format!="vst3" || validate==nullptr)) {
            result.error="unsupported scanner format/validator";return result;
        }
        std::uintmax_t helperSize=0;
        std::filesystem::file_time_type helperTime;
        try {
            helperSize=std::filesystem::file_size(config_.executable);
            helperTime=std::filesystem::last_write_time(config_.executable);
            if(helperSize!=helperSize_ || helperTime!=helperTime_) {
                const auto build=media::blake3File(config_.executable);
                require(static_cast<bool>(build) && helperSize==std::filesystem::file_size(config_.executable) &&
                        helperTime==std::filesystem::last_write_time(config_.executable),"scanner helper changed during identification");
                helperBuildHash_=build.hex;helperSize_=helperSize;helperTime_=helperTime;
            }
        } catch(const std::exception&) {
            result.state=State::HelperCouldNotStart;
            result.error=helperCouldNotStart;
            return result;
        }
        std::error_code ec;const auto canonical=std::filesystem::canonical(path,ec);
        if(ec){result.state=State::Unavailable;result.error="scan candidate path unavailable";return result;}
        result.path=utf8(canonical);require(identity(format,result.path),"invalid canonical scan identity");
        const auto stamp=candidateStamp(format,canonical,helperBuildHash_,result.error);
        if(!stamp){result.cacheWarning=result.error;result.error.clear();}
        std::optional<ScanTicket> ticket;
        auto stable=[&]() {
            const auto after=candidateStamp(format,canonical,helperBuildHash_,result.error);
            std::error_code sizeError,timeError;
            const auto currentSize=std::filesystem::file_size(config_.executable,sizeError);
            const auto currentTime=std::filesystem::last_write_time(config_.executable,timeError);
            if(after && *after==*stamp && !sizeError && !timeError &&
               currentSize==helperSize && currentTime==helperTime)return true;
            std::string finishError;
            if(ticket && !registry_->finishScan(*ticket,ScanDisposition::Abandoned,"",config_.clock(),finishError)) {
                result.state=State::Quarantined;result.error=finishError;
            } else {result.state=State::Unavailable;result.error="scan candidate or helper changed during probe; retry required";}
            return false;
        };
        if(stamp && config_.cacheMode==ScanCacheMode::Reuse) {
            const auto read=registry_->readScanSuccess(format,result.path,*stamp,result.error);
            if(read && read->packet().size()<=config_.process.maximumOutput) {
                auto plugins=decodeMetadata(read->packet(),format,result.path,result.error);
                if(plugins && plugins->empty())plugins.reset();
                if(plugins && validate && !validate(*plugins,result.path,context,result.error))plugins.reset();
                const bool cancelled=config_.process.cancel && config_.process.cancel->load(std::memory_order_relaxed);
                if(cancelled) {
                    // A late block/ticket remains authoritative even when the
                    // validator also cancels. Let ordinary admission decide it.
                    if(registry_->confirmScanSuccess(*read,result.error)) {
                        result.state=State::Cancelled;result.error.clear();return result;
                    }
                } else if(plugins) {
                    if(!stable()) {
                        // Ticketed completion would refuse a late block even
                        // when the candidate changed too. Preserve that priority
                        // without inserting or completing a ticket on this path.
                        std::string blockError;
                        const auto blocked=registry_->hasScanBlock(format,result.path,blockError);
                        if(!blocked || *blocked) {
                            result.state=State::Quarantined;
                            result.error=blocked ? "scan became blocked or pending during validation" : blockError;
                        }
                        return result;
                    }
                    if(registry_->confirmScanSuccess(*read,result.error)) {
                        if(config_.process.cancel && config_.process.cancel->load(std::memory_order_relaxed)) {
                            result.state=State::Cancelled;return result;
                        }
                        result.state=State::Ready;result.cacheHit=true;result.plugins=std::move(*plugins);return result;
                    }
                }
            }
            // Misses, blocked/changed rows and malformed metadata retain the
            // original ticketed admission/repair path and its failure policy.
            result.error.clear();
        }
        std::optional<ScanFailure> cachedFailure;
        if(stamp || config_.cacheMode==ScanCacheMode::Clean) {
            const std::string emptyStamp;
            auto admission=registry_->beginStampedScan(format,result.path,stamp?*stamp:emptyStamp,
                                                        config_.clock(),result.error,config_.cacheMode);
            if(admission){ticket=std::move(admission->ticket);cachedFailure=std::move(admission->cachedFailure);}
        } else ticket=registry_->beginScan(format,result.path,config_.clock(),result.error);
        if(!ticket && !cachedFailure){result.state=State::Quarantined;return result;}
        if(cachedFailure) {
            if(!stable())return result;
            if(config_.process.cancel && config_.process.cancel->load(std::memory_order_relaxed)) {
                result.state=State::Cancelled;return result;
            }
            result.state=cachedFailure->kind==ScanFailureKind::Rejected?State::Failed:State::Quarantined;
            result.failureCacheHit=true;
            result.failureStrikes=cachedFailure->strikes;result.error=cachedFailure->reason;return result;
        }
        auto cached=stamp && config_.cacheMode==ScanCacheMode::Reuse?
            registry_->cachedScan(*ticket,*stamp,result.error):std::nullopt;
        if(!result.error.empty()) {
            std::string finishError;
            if(!registry_->finishScan(*ticket,ScanDisposition::Abandoned,"",config_.clock(),finishError)) {
                result.state=State::Quarantined;result.error=finishError;
            } else result.state=State::Unavailable;
            return result;
        }
        if(cached && cached->size()<=config_.process.maximumOutput) {
            auto plugins=decodeMetadata(*cached,format,result.path,result.error);
            if(plugins && plugins->empty())plugins.reset();
            if(plugins && validate && !validate(*plugins,result.path,context,result.error))plugins.reset();
            if(plugins) {
                if(config_.process.cancel && config_.process.cancel->load(std::memory_order_relaxed)) {
                    result.state=registry_->finishScan(*ticket,ScanDisposition::Abandoned,"",config_.clock(),result.error)?
                        State::Cancelled:State::Quarantined;return result;
                }
                if(!stable())return result;
                if(!registry_->finishScan(*ticket,ScanDisposition::Succeeded,"",config_.clock(),result.error)) {
                    result.state=State::Quarantined;return result;
                }
                result.state=State::Ready;result.cacheHit=true;result.plugins=std::move(*plugins);return result;
            }
            // Corrupt/obsolete stored metadata is not a plug-in crash. Reprobe
            // under the same admission ticket; fresh replies still validate.
            result.error.clear();
        }
        const auto process=runProcess(config_.executable,{"--scan",format,result.path},config_.process);
        const bool cancel=process.succeeded() && config_.process.cancel && config_.process.cancel->load(std::memory_order_relaxed);
        // These exits can also come from a plug-in during load. The prefix
        // only indicates that the helper had passed its pre-open setup.
        const bool enteredProbe = !process.output.empty() && process.output.front()==kModuleOpening;
        const bool setupExit = !enteredProbe &&
            (process.exitCode==64 || process.exitCode==74 || process.exitCode==75 || process.exitCode==78);
        // A partial payload can only follow the helper's enumeration phase.
        const bool writeExit = process.exitCode==75 && enteredProbe && process.output.size()>1;
        const bool helperFailure = process.status==ProcessStatus::Exited && (setupExit || writeExit);
        const bool abandoned=cancel || process.status==ProcessStatus::Cancelled || process.status==ProcessStatus::LaunchFailed ||
            process.status==ProcessStatus::Invalid || process.status==ProcessStatus::CallbackRefused ||
            process.status==ProcessStatus::IoFailed || helperFailure;
        if(abandoned) {
            auto state=State::Unavailable;
            if(cancel || process.status==ProcessStatus::Cancelled)state=State::Cancelled;
            else if(process.status==ProcessStatus::LaunchFailed)state=State::HelperCouldNotStart;
            std::string error;
            if(!registry_->finishScan(*ticket,ScanDisposition::Abandoned,"",config_.clock(),error)) {
                result.state=State::Quarantined;result.error=error;return result;
            }
            result.state=state;
            result.error=state==State::HelperCouldNotStart?helperCouldNotStart:"scanner helper unavailable or scan cancelled";
            return result;
        }
        std::optional<std::vector<Plugin>> plugins;
        if(process.succeeded())plugins=decodeMetadata(process.output,format,result.path,result.error);
        else result.error=failure(process);
        if(plugins && plugins->empty()){plugins.reset();result.error="scanner returned no plug-in classes";}
        if(plugins && validate && !validate(*plugins,result.path,context,result.error))plugins.reset();
        if(config_.process.cancel && config_.process.cancel->load(std::memory_order_relaxed)) {
            result.state=registry_->finishScan(*ticket,ScanDisposition::Abandoned,"",config_.clock(),result.error)?
                State::Cancelled:State::Quarantined;return result;
        }
        if(plugins) {
            if(config_.process.cancel && config_.process.cancel->load(std::memory_order_relaxed)) {
                result.state=registry_->finishScan(*ticket,ScanDisposition::Abandoned,"",config_.clock(),result.error)?
                    State::Cancelled:State::Quarantined;return result;
            }
            if(stamp && !stable())return result;
            const bool finished=stamp?
                registry_->finishCachedScan(*ticket,*stamp,process.output,config_.clock(),result.error):
                registry_->finishScan(*ticket,ScanDisposition::UnstampedSuccess,"",config_.clock(),result.error);
            if(!finished) {
                result.state=State::Quarantined;return result;
            }
            result.state=State::Ready;result.plugins=std::move(*plugins);return result;
        }
        if(result.error.empty())result.error="invalid scanner format data";
        // Only bounded known diagnostics enter durable shared data. A validator
        // may return arbitrary diagnostic text; use a stable fallback if needed.
        if(!text(result.error,4096))result.error="scanner format validation failed";
        const auto diagnostic=result.error;
        if(stamp && !stable())return result;
        result.error=diagnostic;
        std::string finishError;
        bool finished=false;
        const auto kind=process.status==ProcessStatus::Crashed?ScanFailureKind::Crashed:
            (process.status==ProcessStatus::TimedOut?ScanFailureKind::TimedOut:ScanFailureKind::Rejected);
        if(stamp) {
            const auto strikes=registry_->finishFailedScan(*ticket,*stamp,kind,result.error,config_.clock(),finishError);
            finished=strikes.has_value();if(strikes)result.failureStrikes=*strikes;
        } else finished=kind==ScanFailureKind::Rejected?
            registry_->finishScan(*ticket,ScanDisposition::Rejected,"",config_.clock(),finishError):
            registry_->finishScan(*ticket,ScanDisposition::Failed,result.error,config_.clock(),finishError);
        if(!finished) {
            result.state=State::Quarantined;result.error=finishError;return result;
        }
        result.state=State::Failed;return result;
    } catch(const std::exception&) {result.state=State::Unavailable;result.error="scan service exception; pending quarantine retained";return result;}
}
} // namespace adi::scan
