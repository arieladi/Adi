// SPDX-License-Identifier: GPL-3.0-or-later
#include "index.hpp"
#include "adi/media/media_ops.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <utf8proc.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
namespace adi::library {
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {
using Key = std::pair<std::string, std::string>;
std::string normalized(const std::string& s, bool sensitive) {
    utf8proc_uint8_t* data = nullptr;
    const auto flags = static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | (sensitive ? 0 : UTF8PROC_CASEFOLD));
    if (s.size() > static_cast<std::size_t>(std::numeric_limits<utf8proc_ssize_t>::max())) throw std::runtime_error("name too long");
    const auto n = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(s.data()), static_cast<utf8proc_ssize_t>(s.size()), &data, flags);
    std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owned(data, &std::free);
    if (n < 0) throw std::runtime_error("invalid UTF-8 name");
    return {reinterpret_cast<const char*>(data), static_cast<std::size_t>(n)};
}
bool validHash(const std::string& hash) {
    return hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos;
}
struct Metadata { std::int64_t size, time; };
Metadata metadata(const fs::path& path) {
    const auto size = fs::file_size(path);
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max())) throw std::runtime_error("file too large");
    const auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(fs::file_time_type::clock::to_sys(fs::last_write_time(path)).time_since_epoch()).count();
    return {static_cast<std::int64_t>(size), time};
}
bool sameTime(std::int64_t a, std::int64_t b) {
    // Avoid signed subtraction overflow for malformed/extreme timestamps.
    const auto distance = a >= b ? static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)
                                 : static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a);
    return distance <= 2000000000ULL;
}
std::string relativeText(const fs::path& p) {
    if (p.is_absolute()) throw std::runtime_error("content folder must be relative to volume root");
    auto clean = p.lexically_normal();
    while (!clean.empty() && clean.filename().empty()) clean = clean.parent_path();
    for (const auto& part : clean) if (part == "..") throw std::runtime_error("content folder escapes volume");
    return clean.empty() || clean == "." ? "" : media::pathUtf8(clean);
}
}
struct Index::Impl {
    std::unique_ptr<SQLite::Database> db;
    fs::path path;
    Hasher hash;
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::condition_variable cv;
    std::deque<Key> queue;
    std::set<Key> queued;
    std::jthread worker;
    WorkerStats counts;
    bool active = false;
    void enqueue(const Key& key) { if (queued.insert(key).second) queue.push_back(key); }
    void run(std::stop_token stop) {
        const bool priority = lowerWorkerPriority();
        std::unique_lock lock(mutex); counts.lowPriority = priority;
        while (!stop.stop_requested()) {
            cv.wait(lock, [&] { return stop.stop_requested() || !queue.empty(); });
            if (stop.stop_requested()) break;
            const auto key = queue.front(); queue.pop_front(); active = true; bool rescheduled = false;
            try {
                fs::path source; std::int64_t size = 0, time = 0, generation = 0; bool found = false;
                {
                    SQLite::Statement q(*db, "SELECT v.root,f.rel_path,f.size,f.mtime,f.generation FROM files f JOIN volumes v ON f.volume=v.id WHERE f.volume=? AND f.path_key=? AND f.hash IS NULL AND f.present=1");
                    q.bind(1,key.first); q.bind(2,key.second);
                    if (q.executeStep()) {
                        source = media::pathFromUtf8(q.getColumn(0).getString()) / media::pathFromUtf8(q.getColumn(1).getString());
                        size = q.getColumn(2).getInt64(); time = q.getColumn(3).getInt64(); generation = q.getColumn(4).getInt64(); found = true;
                    }
                }
                if (found) {
                    lock.unlock();
                    media::HashResult result; std::string failure;
                    try {
                        const auto before = metadata(source);
                        if (before.size != size || before.time != time) throw std::runtime_error("file changed before hashing; rescan needed");
                        result = hash(source, stop);
                        if (!result) throw std::runtime_error(stop.stop_requested() ? "cancelled" : "hash read failed");
                        const auto after = metadata(source);
                        if (after.size != size || after.time != time) throw std::runtime_error("file changed during hashing; rescan needed");
                        if (!validHash(result.hex)) throw std::runtime_error("hasher returned invalid digest");
                    } catch (const std::exception& e) { failure = e.what(); }
                    lock.lock();
                    if (!stop.stop_requested()) {
                        if (failure.empty()) ++counts.hashed; else ++counts.failed;
                        SQLite::Statement update(*db,"UPDATE files SET hash=?,error=? WHERE volume=? AND path_key=? AND generation=? AND present=1");
                        if (failure.empty()) update.bind(1,result.hex); else update.bind(1);
                        update.bind(2,failure); update.bind(3,key.first); update.bind(4,key.second); update.bind(5,generation); update.exec();
                        // A concurrent scan changed this row; schedule its new metadata.
                        SQLite::Statement again(*db,"SELECT 1 FROM files WHERE volume=? AND path_key=? AND generation<>? AND hash IS NULL AND present=1");
                        again.bind(1,key.first); again.bind(2,key.second); again.bind(3,generation);
                        if (again.executeStep()) { queued.erase(key); enqueue(key); rescheduled = true; }
                    }
                }
            } catch (const std::exception&) { ++counts.failed; }
            // Keep a rescheduled key in the set while its queued copy remains.
            if (!rescheduled) queued.erase(key);
            active = false; cv.notify_all();
        }
        active = false; cv.notify_all();
    }
};
Index::Index(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
Index::~Index() { cancel(); }
std::unique_ptr<Index> Index::open(const fs::path& path, std::string& error, bool autoStart, Hasher testHasher) {
    try {
        if (path.empty()) throw std::runtime_error("library path is empty");
        const auto absolute = fs::absolute(path); fs::create_directories(absolute.parent_path());
        auto p = std::make_unique<Impl>(); p->path = fs::weakly_canonical(absolute);
        p->hash = testHasher ? std::move(testHasher) : Hasher([](const fs::path& file, std::stop_token stop) { return media::blake3File(file,stop); });
        p->db = std::make_unique<SQLite::Database>(media::pathUtf8(absolute),SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        p->db->setBusyTimeout(5000);
        const auto app = p->db->execAndGet("PRAGMA application_id").getInt();
        const auto ver = p->db->execAndGet("PRAGMA user_version").getInt();
        const bool fresh = app == 0 && ver == 0 && p->db->execAndGet("SELECT COUNT(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'").getInt() == 0;
        if (!fresh && (app != applicationId || ver != version)) throw std::runtime_error("not a supported library database");
        if (fresh) {
            SQLite::Transaction tx(*p->db);
            p->db->exec("CREATE TABLE volumes(id TEXT PRIMARY KEY,root TEXT NOT NULL,case_sensitive INTEGER NOT NULL) STRICT;"
                "CREATE TABLE files(volume TEXT NOT NULL REFERENCES volumes(id),path_key TEXT NOT NULL,rel_path TEXT NOT NULL,size INTEGER NOT NULL,mtime INTEGER NOT NULL,hash TEXT,error TEXT NOT NULL DEFAULT '',present INTEGER NOT NULL DEFAULT 1,generation INTEGER NOT NULL DEFAULT 1,PRIMARY KEY(volume,path_key)) STRICT, WITHOUT ROWID;"
                "CREATE INDEX files_hash ON files(hash);"
                "CREATE TABLE annotations(hash TEXT PRIMARY KEY,payload TEXT NOT NULL) STRICT;"
                "PRAGMA application_id=1094994252; PRAGMA user_version=1;");
            tx.commit();
        }
        p->db->exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;");
        auto result = std::unique_ptr<Index>(new Index(std::move(p)));
        if (autoStart) result->start();
        error.clear(); return result;
    } catch (const std::exception& e) { error=e.what(); return {}; }
}
void Index::start() {
    std::lock_guard life(impl_->lifecycle); std::lock_guard lock(impl_->mutex);
    if (impl_->worker.joinable()) return;
    SQLite::Statement pending(*impl_->db,"SELECT volume,path_key FROM files WHERE hash IS NULL AND present=1");
    while (pending.executeStep()) impl_->enqueue({pending.getColumn(0).getString(),pending.getColumn(1).getString()});
    impl_->worker = std::jthread([this](std::stop_token stop){ impl_->run(stop); });
}
void Index::cancel() {
    std::lock_guard life(impl_->lifecycle);
    if (impl_->worker.joinable()) {
        // Change the wait predicate under its mutex: otherwise notification
        // can arrive between predicate evaluation and entering cv.wait.
        { std::lock_guard lock(impl_->mutex); impl_->worker.request_stop(); }
        impl_->cv.notify_all(); impl_->worker.join();
    }
    std::lock_guard lock(impl_->mutex); impl_->queue.clear(); impl_->queued.clear(); impl_->cv.notify_all();
}
void Index::waitIdle() {
    std::unique_lock lock(impl_->mutex);
    impl_->cv.wait(lock,[&]{return impl_->queue.empty() && !impl_->active;});
}
WorkerStats Index::stats() const { std::lock_guard lock(impl_->mutex); return impl_->counts; }
ScanResult Index::scan(const Volume& volume, const fs::path& relativeFolder) {
    ScanResult result;
    try {
        if (volume.id.empty()) throw std::runtime_error("volume identity is empty");
        const auto folder = relativeText(relativeFolder);
        const auto root = fs::canonical(volume.root);
        const auto start = folder.empty() ? root : root / media::pathFromUtf8(folder);
        auto componentPath = root;
        for (const auto& part : media::pathFromUtf8(folder)) {
            componentPath /= part;
            if (fs::is_symlink(fs::symlink_status(componentPath))) throw std::runtime_error("content folder traverses symlink");
        }
        struct Item { std::string path,key; Metadata meta; };
        std::vector<Item> items; std::set<std::string> keys;
        for (const auto& file : fs::recursive_directory_iterator(start)) {
            if (file.is_symlink() || !file.is_regular_file()) continue;
            const auto path = file.path(); const auto rel = media::pathUtf8(path.lexically_relative(root));
            if (rel == ".adi-volume-id" || rel.starts_with(".adi-volume-probe-")) continue;
            const auto full = media::pathUtf8(path);
            const auto database = media::pathUtf8(impl_->path);
            if (full == database || full == database+"-wal" || full == database+"-shm") continue;
            const auto key = normalized(rel,volume.caseSensitive);
            if (!keys.insert(key).second) throw std::runtime_error("ambiguous normalized filenames: " + rel);
            items.push_back({rel,key,metadata(path)});
        }
        std::lock_guard lock(impl_->mutex); SQLite::Transaction tx(*impl_->db);
        {
            SQLite::Statement existing(*impl_->db,"SELECT case_sensitive FROM volumes WHERE id=?"); existing.bind(1,volume.id);
            if (existing.executeStep() && (existing.getColumn(0).getInt()!=0) != volume.caseSensitive) throw std::runtime_error("volume case behavior changed; explicit rebuild needed");
            SQLite::Statement q(*impl_->db,"INSERT INTO volumes VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET root=excluded.root WHERE root<>excluded.root");
            q.bind(1,volume.id); q.bind(2,media::pathUtf8(root)); q.bind(3,volume.caseSensitive?1:0); q.exec();
        }
        const auto prefix = folder.empty() ? std::string{} : normalized(folder,volume.caseSensitive)+"/";
        SQLite::Statement old(*impl_->db,"SELECT size,mtime,hash,rel_path,present FROM files WHERE volume=? AND path_key=?");
        SQLite::Statement keep(*impl_->db,"UPDATE files SET rel_path=?,present=1 WHERE volume=? AND path_key=?");
        SQLite::Statement change(*impl_->db,"INSERT INTO files(volume,path_key,rel_path,size,mtime) VALUES(?,?,?,?,?) ON CONFLICT(volume,path_key) DO UPDATE SET rel_path=excluded.rel_path,size=excluded.size,mtime=excluded.mtime,hash=NULL,error='',present=1,generation=files.generation+1");
        std::vector<Key> pending;
        for (const auto& item : items) {
            ++result.files; old.reset(); old.clearBindings(); old.bind(1,volume.id); old.bind(2,item.key);
            const bool found = old.executeStep();
            const bool unchanged = found && !old.getColumn(2).isNull() && old.getColumn(0).getInt64()==item.meta.size && sameTime(old.getColumn(1).getInt64(),item.meta.time);
            if (unchanged) {
                ++result.unchanged;
                if (old.getColumn(3).getString()!=item.path || old.getColumn(4).getInt()==0) {
                    keep.reset(); keep.bind(1,item.path); keep.bind(2,volume.id); keep.bind(3,item.key); keep.exec();
                }
            } else {
                change.reset(); change.bind(1,volume.id); change.bind(2,item.key); change.bind(3,item.path); change.bind(4,item.meta.size); change.bind(5,item.meta.time); change.exec();
                pending.emplace_back(volume.id,item.key); ++result.queued;
            }
        }
        // Range uses the composite primary key, including for a partial scan.
        // A trailing slash's successor is '0'; all descendants sort between.
        std::string upper = prefix;
        if (!upper.empty()) upper.back() = '0';
        SQLite::Statement prior(*impl_->db, prefix.empty()
            ? "SELECT path_key FROM files WHERE volume=? AND present=1"
            : "SELECT path_key FROM files WHERE volume=? AND path_key>=? AND path_key<? AND present=1");
        prior.bind(1,volume.id);
        if (!prefix.empty()) { prior.bind(2,prefix); prior.bind(3,upper); }
        SQLite::Statement missing(*impl_->db,"UPDATE files SET present=0 WHERE volume=? AND path_key=?");
        while (prior.executeStep()) {
            const auto key=prior.getColumn(0).getString();
            if (!keys.contains(key)) { missing.reset(); missing.bind(1,volume.id); missing.bind(2,key); missing.exec(); }
        }
        tx.commit();
        for (const auto& key : pending) impl_->enqueue(key);
        result.ok=true; impl_->cv.notify_all(); return result;
    } catch (const std::exception& e) { result.error=e.what(); return result; }
}
std::vector<Entry> Index::entries() const {
    std::lock_guard lock(impl_->mutex); std::vector<Entry> result;
    SQLite::Statement q(*impl_->db,"SELECT volume,rel_path,size,mtime,coalesce(hash,''),error,present FROM files ORDER BY volume,path_key");
    while(q.executeStep()) result.push_back({q.getColumn(0).getString(),q.getColumn(1).getString(),q.getColumn(4).getString(),q.getColumn(5).getString(),static_cast<std::uintmax_t>(q.getColumn(2).getInt64()),q.getColumn(3).getInt64(),q.getColumn(6).getInt()!=0});
    return result;
}
std::string Index::exportJson() const {
    std::lock_guard lock(impl_->mutex); Json media = Json::object();
    SQLite::Statement q(*impl_->db,"SELECT hash,payload FROM annotations ORDER BY hash");
    while(q.executeStep()) media[q.getColumn(0).getString()] = Json::parse(q.getColumn(1).getString());
    return Json{{"version",1},{"media",media}}.dump(2);
}
bool Index::importJson(const std::string& text,std::string& error) {
    try {
        const auto document=Json::parse(text);
        if (!document.is_object() || document.size()!=2 || document.at("version")!=1 || !document.at("media").is_object()) throw std::runtime_error("invalid library JSON envelope");
        std::lock_guard lock(impl_->mutex); SQLite::Transaction tx(*impl_->db);
        for (const auto& [hash, incoming] : document.at("media").items()) {
            if (!validHash(hash) || !incoming.is_object()) throw std::runtime_error("metadata must be keyed by BLAKE3 hash");
            Json merged={{"tags",Json::array()},{"bpm",nullptr},{"rating",nullptr}};
            SQLite::Statement old(*impl_->db,"SELECT payload FROM annotations WHERE hash=?"); old.bind(1,hash); if(old.executeStep()) merged=Json::parse(old.getColumn(0).getString());
            for(const auto& [key,value] : incoming.items()) {
                if (key=="tags") {
                    if(!value.is_array()) throw std::runtime_error("tags must be an array");
                    std::set<std::string> tags=merged.at("tags").get<std::set<std::string>>();
                    for(const auto& tag:value) { if(!tag.is_string()) throw std::runtime_error("tag must be text"); tags.insert(normalized(tag.get<std::string>(),true)); }
                    merged["tags"]=tags;
                } else if(key=="bpm") {
                    if(!value.is_null() && (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>()<=0 || value.get<double>()>1000)) throw std::runtime_error("invalid BPM");
                    merged[key]=value;
                } else if(key=="rating") {
                    if(!value.is_null() && (!value.is_number_integer() || value.get<std::int64_t>()<0 || value.get<std::int64_t>()>5)) throw std::runtime_error("invalid rating");
                    merged[key]=value;
                } else throw std::runtime_error("unknown metadata field");
            }
            SQLite::Statement put(*impl_->db,"INSERT INTO annotations VALUES(?,?) ON CONFLICT(hash) DO UPDATE SET payload=excluded.payload"); put.bind(1,hash); put.bind(2,merged.dump()); put.exec();
        }
        tx.commit(); error.clear(); return true;
    } catch(const std::exception& e) { error=e.what(); return false; }
}
}
