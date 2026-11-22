// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/scan_prune.hpp"
#include "adi/plugin_registry.hpp"
#include "adi/scan_registry_schema.hpp"
#include "adi/audio/io_audit.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <system_error>

namespace adi::scan {
namespace {
namespace fs = std::filesystem;
struct Refusal : std::runtime_error {
    ScanPruneStatus status;
    Refusal(ScanPruneStatus s, const std::string& why) : std::runtime_error(why), status(s) {}
};
std::string utf8(const fs::path& path) { const auto u=path.u8string();return {u.begin(),u.end()}; }
void stop(const ScanPruneRequest& request) {
    if(request.cancel && request.cancel->load())throw Refusal(ScanPruneStatus::Cancelled,"scan pruning cancelled");
}
bool absent(const std::error_code& ec, const fs::file_status& status) {
    return status.type()==fs::file_type::not_found && (!ec || ec==std::errc::no_such_file_or_directory);
}
bool alias(const fs::file_status& status) {
    if(fs::is_symlink(status))return true;
#if defined(_MSVC_STL_VERSION)
    // MSVC distinguishes native mount-point junctions from symbolic links.
    return status.type()==fs::file_type::junction;
#else
    return false;
#endif
}
bool within(const fs::path& path, const fs::path& root) {
    auto p=path.begin();
    for(auto r=root.begin();r!=root.end();++r,++p)if(p==path.end() || *p!=*r)return false;
    return p!=path.end(); // a search root itself is never a module to remove
}
struct Root {std::string format;fs::path original,canonical;};
struct Walk {
    std::vector<Root> roots;
    std::vector<ScanPruneRoot> excluded;
    std::set<std::pair<std::string,fs::path>> seen;
};
void rootStable(const Root& root) {
    std::error_code ec;const auto now=fs::canonical(root.original,ec);
    if(ec || now!=root.canonical || !fs::is_directory(now,ec) || ec)
        throw Refusal(ScanPruneStatus::Incomplete,"scan root changed or cannot be checked: "+utf8(root.original));
}
Walk resolveRoots(const ScanPruneRequest& request, ScanPruneResult& result) {
    if(request.roots.size()>1024)throw Refusal(ScanPruneStatus::Invalid,"too many scan roots");
    Walk out;
    for(const auto& root:request.roots) {
        stop(request);
        if((root.format!="clap" && root.format!="vst3") || root.path.empty() ||
            utf8(root.path).find('\0')!=std::string::npos)
            throw Refusal(ScanPruneStatus::Invalid,"invalid scan pruning root");
        std::error_code ec;const auto original=fs::absolute(root.path,ec).lexically_normal();
        if(ec)throw Refusal(ScanPruneStatus::Incomplete,"cannot resolve scan root: "+utf8(root.path));
        const auto status=fs::symlink_status(original,ec);
        if(absent(ec,status)) {
            ++result.missingRootsSkipped;
            auto excluded=fs::weakly_canonical(original,ec);
            out.excluded.push_back({root.format,ec?original:excluded});
            continue;
        }
        if(ec)throw Refusal(ScanPruneStatus::Incomplete,"cannot inspect scan root: "+utf8(original)+": "+ec.message());
        const auto canonical=fs::canonical(original,ec);
        if(ec==std::errc::no_such_file_or_directory) {
            ++result.missingRootsSkipped;out.excluded.push_back({root.format,original});continue;
        }
        if(ec || !fs::is_directory(canonical,ec) || ec)
            throw Refusal(ScanPruneStatus::Incomplete,"scan root is not an accessible directory: "+utf8(original));
        if(std::none_of(out.roots.begin(),out.roots.end(),[&](const Root& r){return r.format==root.format && r.canonical==canonical;}))
            out.roots.push_back({root.format,original,canonical});
    }
    return out;
}
Walk walk(const ScanPruneRequest& request,Walk out, ScanPruneResult& result) {
    std::size_t entries=0;
    for(const auto& root:out.roots) {
        std::vector<fs::path> pending{root.canonical};std::set<fs::path> visited;
        while(!pending.empty()) {
            stop(request);auto dir=std::move(pending.back());pending.pop_back();
            if(!visited.insert(dir).second)continue;
            std::error_code ec;fs::directory_iterator it(dir,fs::directory_options::none,ec),end;
            if(ec)throw Refusal(ScanPruneStatus::Incomplete,"cannot enumerate scan directory: "+utf8(dir)+": "+ec.message());
            for(;it!=end;it.increment(ec)) {
                if(ec)break;
                stop(request);
                if(++entries>262144)throw Refusal(ScanPruneStatus::Incomplete,"scan pruning walk bound reached");
                const auto path=it->path();
                auto extension=utf8(path.extension());
                for(char& c:extension)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');
                const bool candidate=extension=="."+root.format;
                const auto entryStatus=it->symlink_status(ec);
                if(ec)throw Refusal(ScanPruneStatus::Incomplete,"scan entry cannot be inspected: "+utf8(path)+": "+ec.message());
                // Directory enumeration already supplies ordinary type info.
                // Unrelated regular files need no canonicalization/seen entry.
                if(!candidate && !fs::is_directory(entryStatus) && !alias(entryStatus))continue;
                const auto canonical=fs::canonical(path,ec);
                if(ec==std::errc::no_such_file_or_directory && alias(entryStatus)) {
                    auto target=fs::read_symlink(path,ec);
                    if(ec)throw Refusal(ScanPruneStatus::Incomplete,"dangling scan alias target cannot be read: "+utf8(path)+": "+ec.message());
                    if(target.is_relative())target=path.parent_path()/target;
                    target=fs::weakly_canonical(target,ec);
                    if(ec)throw Refusal(ScanPruneStatus::Incomplete,"dangling scan alias target cannot be resolved: "+utf8(path)+": "+ec.message());
                    out.excluded.push_back({root.format,path});out.excluded.push_back({root.format,target});
                    continue; // confirmed dangling aliases reserve their old scope; they are not a partial walk
                }
                if(ec)throw Refusal(ScanPruneStatus::Incomplete,"scan entry cannot be resolved: "+utf8(path)+": "+ec.message());
                if(!within(canonical,root.canonical))continue; // alias targets outside this walked root are not owned
                if(candidate){out.seen.emplace(root.format,canonical);continue;} // module/bundle leaf
                const bool directory=it->is_directory(ec);
                if(ec)throw Refusal(ScanPruneStatus::Incomplete,"scan entry cannot be inspected: "+utf8(path)+": "+ec.message());
                if(directory)pending.push_back(canonical);
            }
            if(ec)throw Refusal(ScanPruneStatus::Incomplete,"scan directory traversal failed: "+utf8(dir)+": "+ec.message());
        }
        rootStable(root);++result.rootsWalked;
    }
    return out;
}
struct Key {std::string format,text;fs::path path;};
bool missingOwned(const Key& key,const Walk& walked,ScanPruneResult& result) {
    if(walked.seen.contains({key.format,key.path}))return false;
    for(const auto& exclusion:walked.excluded)
        if(key.format==exclusion.format && (key.path==exclusion.path || within(key.path,exclusion.path)))return false;
    const auto root=std::find_if(walked.roots.begin(),walked.roots.end(),[&](const Root& r){return r.format==key.format && within(key.path,r.canonical);});
    if(root==walked.roots.end())return false;
    rootStable(*root);
    std::error_code ec;const auto status=fs::symlink_status(key.path,ec);
    if(!absent(ec,status)) {
        if(ec)throw Refusal(ScanPruneStatus::Incomplete,"cached module cannot be inspected: "+key.text+": "+ec.message());
        return false;
    }
    // A vanished canonical key under a parent now replaced by an alias is not
    // evidence about its old target. Retain it even if that alias stays in-root.
    const auto resolved=fs::weakly_canonical(key.path,ec);
    if(ec || resolved!=key.path){++result.unverifiableKeysSkipped;return false;}
    return true;
}
bool schema(SQLite::Database& db) {
    if(db.execAndGet("PRAGMA application_id").getInt()!=PluginRegistry::kApplicationId)
        throw Refusal(ScanPruneStatus::Failed,"file is not a plugin registry");
    const auto version=db.execAndGet("PRAGMA user_version").getInt();
    if(version==1 || version==2){detail::validateLegacy(db,version);return false;}
    if(version!=PluginRegistry::kVersion)
        throw Refusal(ScanPruneStatus::Failed,"pruning requires the current plugin registry schema");
    detail::tableExists(db,"plugin_scan_cache",true);detail::tableExists(db,"plugin_scan_blocks",true);
    SQLite::Statement cache(db,"SELECT format,path,stamp,packet,updated_utc FROM plugin_scan_cache LIMIT 0");cache.executeStep();
    SQLite::Statement blocks(db,"SELECT id,format,path,state,reason,updated_utc FROM plugin_scan_blocks LIMIT 0");blocks.executeStep();
    return detail::tableExists(db,"plugin_scan_failures",false);
}
void bind(SQLite::Statement& query,const Key& key) {query.bind(1,key.format);query.bind(2,key.text);}
} // namespace

struct ScanPruneBinding {std::vector<ScanPruneRoot> requested;Walk initial;};
ScanPruneResult bindScanPruneRoots(ScanPruneRequest& request) {
    ScanPruneResult result;
    if(audio::onAudioThread){result.status=ScanPruneStatus::CallbackRefused;return result;}
    request.binding.reset();
    try {
        stop(request);auto resolved=resolveRoots(request,result);stop(request);
        auto binding=std::make_shared<ScanPruneBinding>();binding->requested=request.roots;binding->initial=std::move(resolved);
        request.binding=std::move(binding);result.status=ScanPruneStatus::Complete;
    }catch(const Refusal& e){result.status=e.status;result.error=e.what();}
    catch(const std::exception& e){result.status=ScanPruneStatus::Failed;result.error=e.what();}
    catch(...){result.status=ScanPruneStatus::Failed;result.error="scan root binding failed";}
    return result;
}

ScanPruneResult pruneMissingScans(const std::filesystem::path& registryPath,const ScanPruneRequest& request) {
    ScanPruneResult result;
    if(audio::onAudioThread){result.status=ScanPruneStatus::CallbackRefused;return result;}
    try {
        stop(request);
        if(!request.discoveryComplete){result.error="discovery is incomplete; no scan metadata pruned";return result;}
        if(!request.binding)throw Refusal(ScanPruneStatus::Incomplete,"scan roots were not bound before discovery");
        const auto equalRoot=[](const ScanPruneRoot& a,const ScanPruneRoot& b){return a.format==b.format && a.path==b.path;};
        const auto& binding=*request.binding;
        if(!std::equal(request.roots.begin(),request.roots.end(),binding.requested.begin(),binding.requested.end(),equalRoot))
            throw Refusal(ScanPruneStatus::Incomplete,"configured scan roots changed after binding");
        auto current=resolveRoots(request,result);
        const auto equalDirectory=[](const Root& a,const Root& b){return a.format==b.format && a.original==b.original && a.canonical==b.canonical;};
        if(!std::equal(current.roots.begin(),current.roots.end(),binding.initial.roots.begin(),binding.initial.roots.end(),equalDirectory) ||
           !std::equal(current.excluded.begin(),current.excluded.end(),binding.initial.excluded.begin(),binding.initial.excluded.end(),equalRoot))
            throw Refusal(ScanPruneStatus::Incomplete,"scan root binding changed during discovery");
        const auto walked=walk(request,std::move(current),result);
        if(walked.roots.empty()){result.status=ScanPruneStatus::Complete;return result;}
        if(request.checkpoint)request.checkpoint(ScanPrunePhase::AfterWalk,{},request.context);
        stop(request);for(const auto& root:walked.roots)rootStable(root);
        if(registryPath.empty() || !registryPath.is_absolute() || utf8(registryPath).find('\0')!=std::string::npos)
            throw Refusal(ScanPruneStatus::Invalid,"invalid plugin registry path");
        std::error_code ec;const auto registryStatus=fs::symlink_status(registryPath,ec);
        if(absent(ec,registryStatus)){result.status=ScanPruneStatus::Complete;return result;}
        if(ec || !fs::is_regular_file(registryStatus))throw Refusal(ScanPruneStatus::Failed,"plugin registry is not an accessible regular file");
        std::vector<Key> candidates;
        {
            SQLite::Database reader(utf8(registryPath),SQLite::OPEN_READONLY);
            if(!schema(reader)){result.legacySkipped=true;result.status=ScanPruneStatus::Complete;return result;}
            SQLite::Statement rows(reader,"SELECT format,path,length(CAST(path AS BLOB)) FROM ("
                "SELECT format,path FROM plugin_scan_cache UNION SELECT format,path FROM plugin_scan_failures)");
            std::size_t count=0,bytes=0;
            while(rows.executeStep()) {
                stop(request);
                if(++count>131072 || !rows.getColumn(0).isText() || !rows.getColumn(1).isText() ||
                    rows.getColumn(2).getInt64()<1 || rows.getColumn(2).getInt64()>65536)
                    throw Refusal(ScanPruneStatus::Failed,"invalid or excessive cached scan key inventory");
                Key key{rows.getColumn(0).getString(),rows.getColumn(1).getString(),{}};
                bytes+=key.text.size();
                if(bytes>16U*1024U*1024U || (key.format!="clap" && key.format!="vst3") || key.text.find('\0')!=std::string::npos)
                    throw Refusal(ScanPruneStatus::Failed,"invalid or excessive cached scan key text");
                key.path=fs::path(std::u8string(key.text.begin(),key.text.end()));
                if(!key.path.is_absolute() || key.path.lexically_normal()!=key.path)
                    throw Refusal(ScanPruneStatus::Failed,"cached scan key is not normalized and absolute");
                if(missingOwned(key,walked,result))candidates.push_back(std::move(key));
            }
        }
        stop(request);for(const auto& root:walked.roots)rootStable(root);
        if(candidates.empty()){result.status=ScanPruneStatus::Complete;return result;}
        SQLite::Database writer(utf8(registryPath),SQLite::OPEN_READWRITE);writer.setBusyTimeout(2000);
        SQLite::Transaction transaction(writer,SQLite::TransactionBehavior::IMMEDIATE);
        if(!schema(writer))throw Refusal(ScanPruneStatus::Failed,"registry schema changed during pruning");
        std::uint64_t caches=0,failures=0,blocks=0;
        for(const auto& key:candidates) {
            stop(request);
            if(request.checkpoint)request.checkpoint(ScanPrunePhase::BeforeDelete,key.path,request.context);
            stop(request);
            if(!missingOwned(key,walked,result))continue;
            // Only a typed two-strike association owns an automatic block.
            // Pending, manual and legacy blocks shield all metadata for the key.
            SQLite::Statement block(writer,"SELECT b.id,b.state,f.block_id,f.strikes,f.kind FROM plugin_scan_blocks b "
                "LEFT JOIN plugin_scan_failures f ON f.format=b.format AND f.path=b.path WHERE b.format=? AND b.path=?");
            bind(block,key);
            if(block.executeStep()) {
                const auto id=block.getColumn(0).getInt64();
                if(block.getColumn(1).getString()!="blocked" || !block.getColumn(2).isInteger() ||
                    block.getColumn(2).getInt64()!=id || block.getColumn(3).getInt()!=2 ||
                    (block.getColumn(4).getString()!="crashed" && block.getColumn(4).getString()!="timeout"))continue;
                SQLite::Statement ownedFailure(writer,"DELETE FROM plugin_scan_failures WHERE format=? AND path=? AND block_id=?");
                bind(ownedFailure,key);ownedFailure.bind(3,id);
                failures+=static_cast<unsigned>(ownedFailure.exec());
                SQLite::Statement erase(writer,"DELETE FROM plugin_scan_blocks WHERE id=? AND format=? AND path=? AND state='blocked'");
                erase.bind(1,id);erase.bind(2,key.format);erase.bind(3,key.text);
                const auto removed=erase.exec();blocks+=static_cast<unsigned>(removed);
            }
            SQLite::Statement cache(writer,"DELETE FROM plugin_scan_cache WHERE format=? AND path=?");bind(cache,key);
            caches+=static_cast<unsigned>(cache.exec());
            SQLite::Statement failure(writer,"DELETE FROM plugin_scan_failures WHERE format=? AND path=?");bind(failure,key);
            failures+=static_cast<unsigned>(failure.exec());
        }
        stop(request);for(const auto& root:walked.roots)rootStable(root);
        transaction.commit();
        result.cacheRowsRemoved=caches;result.failureRowsRemoved=failures;result.automaticBlocksRemoved=blocks;
        result.status=ScanPruneStatus::Complete;
    }catch(const Refusal& e){result.status=e.status;result.error=e.what();}
    catch(const std::exception& e){result.status=ScanPruneStatus::Failed;result.error=e.what();}
    catch(...){result.status=ScanPruneStatus::Failed;result.error="scan pruning failed";}
    return result;
}
} // namespace adi::scan
