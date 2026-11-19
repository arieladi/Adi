// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/scan_stamp.hpp"
#include "adi/plugin_scanner.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/media/blake3.hpp"
#include <algorithm>
#include <chrono>
#include <span>
#include <set>
#include <stdexcept>
#include <vector>

namespace adi::scan {
namespace {
std::string utf8(const std::filesystem::path& path) {
    const auto bytes=path.u8string();return {bytes.begin(),bytes.end()};
}
void require(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
void field(std::string& into,const std::string& value) {
    require(value.size()<=65536 && into.size()+value.size()+32<=16U*1024U*1024U,
            "scan stamp text bound");
    into+=std::to_string(value.size());into+=':';into+=value;into+='\n';
}
std::string lower(std::string value) {
    for(char& ch:value)if(ch>='A' && ch<='Z')ch=static_cast<char>(ch-'A'+'a');
    return value;
}
bool module(const std::filesystem::path& relative) {
    const auto name=lower(utf8(relative.filename()));
    const auto extension=lower(utf8(relative.extension()));
    const auto generic=relative.generic_u8string();
    return name=="moduleinfo.json" || extension==".vst3" || extension==".clap" ||
           extension==".dll" || extension==".so" || extension==".dylib" ||
           generic.starts_with(u8"Contents/MacOS/");
}
void appendFile(std::string& into,const std::filesystem::path& path,
                const std::filesystem::path& relative) {
    namespace fs=std::filesystem;
    field(into,utf8(relative));
    field(into,utf8(fs::canonical(path)));
    field(into,std::to_string(fs::file_size(path)));
    // In nanoseconds as a long long: the file clock's own count is a 128-bit integer on Apple's
    // library, which std::to_string has no overload for.
    field(into,std::to_string(static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        fs::last_write_time(path).time_since_epoch()).count())));
}
} // namespace

std::optional<std::string> candidateStamp(const std::string& format,
    const std::filesystem::path& canonical,const std::string& helperBuildHash,std::string& error) {
    if(audio::onAudioThread)return std::nullopt;
    error.clear();
    try {
        namespace fs=std::filesystem;
        require((format=="clap" || format=="vst3") && canonical.is_absolute() &&
                helperBuildHash.size()==64,"invalid scan stamp identity");
        std::string bytes;
        field(bytes,"adi.scan.success-stamp/1");field(bytes,std::to_string(kPacketSchema));
        field(bytes,format);field(bytes,helperBuildHash);field(bytes,utf8(canonical));
        if(fs::is_regular_file(canonical))appendFile(bytes,canonical,canonical.filename());
        else {
            require(fs::is_directory(canonical),"scan candidate is not a module or bundle");
            std::vector<fs::path> files,ordinary;
            std::vector<fs::path> pending{canonical};
            std::set<fs::path> visited;
            std::vector<std::pair<std::string,std::string>> links;
            std::size_t entries=0;
            while(!pending.empty()) {
                const auto directory=pending.back();pending.pop_back();
                const auto resolved=fs::canonical(directory);
                const auto within=resolved.lexically_relative(canonical);
                require(!within.empty() && *within.begin()!=fs::path(".."),
                        "bundle directory link leaves module; success cache bypassed");
                links.emplace_back(utf8(directory.lexically_relative(canonical)),utf8(resolved));
                if(!visited.insert(resolved).second)continue;
                std::vector<fs::directory_entry> children;
                for(const auto& entry:fs::directory_iterator(directory)) {
                    require(++entries<=16384,"scan bundle entry bound");children.push_back(entry);
                }
                std::sort(children.begin(),children.end(),[](const auto& a,const auto& b){return a.path()<b.path();});
                // LIFO in reverse order keeps traversal deterministic through
                // aliases to the same directory, irrespective of directory enumeration order.
                for(auto it=children.rbegin();it!=children.rend();++it) {
                    if(it->is_directory())pending.push_back(it->path());
                    else if(it->is_regular_file()) {
                        ordinary.push_back(it->path());
                        if(module(it->path().lexically_relative(canonical))) {
                            require(files.size()<4096,"scan bundle module bound");files.push_back(it->path());
                        }
                    }
                }
            }
            std::sort(links.begin(),links.end());
            for(const auto& [relative,target]:links){field(bytes,relative);field(bytes,target);}
            if(files.empty())files=std::move(ordinary);
            require(files.size()<=4096,"scan bundle fallback file bound");
            std::sort(files.begin(),files.end());
            for(const auto& path:files)appendFile(bytes,path,path.lexically_relative(canonical));
        }
        const auto result=media::blake3Bytes(std::as_bytes(std::span(bytes.data(),bytes.size())));
        require(static_cast<bool>(result),"scan stamp hash unavailable");
        return result.hex;
    } catch(const std::exception& failure) {error=failure.what();return std::nullopt;}
}
} // namespace adi::scan
