// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/store.hpp"
#include "adi/ops.hpp"
#include "adi/history.hpp"
#include "adi/history_compaction.hpp"
#include "adi/digest.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/applied_batches.hpp"
#include "adi/engine/session.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>

namespace {
using namespace adi;
namespace fs=std::filesystem;
int checks=0,failures=0;
void check(bool good,const char* name){++checks;if(!good){++failures;std::printf("FAIL: %s\n",name);}}
struct Fixture {
    test::TempDirectory temp{"project_files","source"};
    std::unique_ptr<Store> store;
    fs::path source,folder;
    explicit Fixture(const fs::path& sample = "audio/tone.wav") {
        folder=temp.path()/"original";fs::create_directories((folder/sample).parent_path());
        source=folder/"Source.adi";StoreError error{};store=Store::createEmpty(source,error);
        if(!store)throw std::runtime_error("create fixture");
        store->db().exec("INSERT INTO project(id,name) VALUES(1,'Original');"
            "INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Audio'),(9,'master','Main');");
        {audio::WavWriter wave(folder/sample,48000,1,audio::WavFormat::Float32);
            std::array<float,512> samples{};samples.fill(0.25f);wave.write(samples.data(),512);}
        OpRequest capture;const auto prepared=media::prepareImport(*store,sample,10,1,capture);
        if(!prepared.ok)throw std::runtime_error(prepared.error);
        OpRequest clip;clip.opType="clip.create";clip.payload={{"id",5},{"track",1},{"kind","audio"},{"pos",0},{"length",5765760}};
        OpRequest attach;attach.opType="clip.attachAudio";attach.payload={{"id",5},{"media",10},{"frames",512}};
        if(!OpJournal(*store).commit(std::vector<OpRequest>{capture,clip,attach}).ok)throw std::runtime_error("import fixture");
    }
    fs::path target(const char* directory,const fs::path& name="Copy.adi") {
        const auto parent=temp.path()/directory;fs::create_directories(parent);return parent/name;
    }
};
std::string historyBytes(Store& s) {
    std::string bytes;SQLite::Statement q(s.db(),"SELECT hex(payload),hex(inverse) FROM ops ORDER BY seq");
    while(q.executeStep())bytes+=q.getColumn(0).getString()+q.getColumn(1).getString();
    return bytes;
}
bool resolves(Store& s) {
    std::string error;return media::resolveMedia(media::projectFolder(s),media::mediaRow(s.db(),10),error).is_absolute();
}
void copiesAndSwitches() {
    Fixture f;const auto before=digestProject(*f.store).text,history=historyBytes(*f.store);
    const auto target=f.target("elsewhere");
    check(fs::exists(media::pathFromUtf8(media::pathUtf8(f.source)+"-wal")),"the source has committed WAL data");
    const auto copied=f.store->saveCopy(target);check(copied.ok,"Save a Copy succeeds from a live source");
    if(!copied.ok){std::printf("%s\n",copied.error.c_str());return;}
    check(f.store->path()==f.source && digestProject(*f.store).text==before && historyBytes(*f.store)==history,
          "Copy preserves the active source, rows and recorded history bytes");
    check(!fs::exists(media::pathFromUtf8(media::pathUtf8(target)+"-wal")),"the published copy is a single file before opening");
    StoreError error{};auto copy=Store::open(target,error);
    check(copy && copy->db().execAndGet("SELECT count(*) FROM audio_clips").getInt()==1 && resolves(*copy),
          "the copy includes committed WAL rows and resolves media from its new folder");
    if(!copy)return;
    check(copy->db().execAndGet("SELECT count(*) FROM ops").getInt()==3,"copy retains the undo transaction without a save op");
    History copiedHistory(*copy);
    check(copiedHistory.undo().ok && copy->db().execAndGet("SELECT count(*) FROM media_files").getInt()==0,
          "copy can undo the imported clip as one step");
    check(copiedHistory.redo().ok && resolves(*copy),"Redo's captured import paths are relocated too");
    {
        engine::Session session;engine::SessionSpec spec;spec.sampleRate=48000;spec.maxFrames=64;
        check(session.load(*copy,{},spec),"relocated copy loads for playback");
        session.transport().play();check(session.clips()->prime(),"relocated media primes");
        std::array<float,64> l{},r{};float* out[]{l.data(),r.data()};engine::AudioIo io;io.out=out;io.numOut=2;io.frames=64;
        session.process(io);
        check(std::all_of(l.begin(),l.end(),[](float v){return v==0.25f;}) && l==r,"relocated audio actually plays");
    }
    check(!f.store->saveCopy(target).ok && copy->db().execAndGet("SELECT name FROM project").getString()=="Original",
          "Copy refuses an existing destination");
    check(!f.store->saveCopy(f.source).ok,"Copy never overwrites its own source");
    copy.reset();
    OpJournal journal(*f.store);History activeHistory(*f.store);
    const auto client=f.store->clientId();
    const auto newPath=f.target("unicode",fs::path(u8"פרויקט.adi"));
    const auto saved=f.store->saveAs(newPath);check(saved.ok,"Save As switches to a Unicode filename");
    if(!saved.ok){std::printf("%s\n",saved.error.c_str());return;}
    check(f.store->path()==newPath && f.store->clientId()==client && !f.store->readOnly(),"Save As retains the same writer identity in the new file");
    OpRequest rename;rename.opType="project.setName";rename.payload={{"name","After Save As"}};
    check(journal.commit(rename).ok && activeHistory.canUndo(),"pre-existing journal/history objects continue editing the new file");
    auto original=Store::open(f.source,error,true);
    check(original && original->db().execAndGet("SELECT name FROM project").getString()=="Original",
          "continuing edits do not touch the original project");
    check(activeHistory.undo().ok && resolves(*f.store),"Undo continues in the new project with working media references");
    original.reset();f.store.reset();
    auto reopened=Store::open(newPath,error);
    check(reopened && resolves(*reopened) && History(*reopened).canUndo(),"Save As and its history survive reopen");
}
void namedSaveAsKeepsMediaReferences() {
    const fs::path recording = "Samples/Recorded/Audio.wav";
    Fixture f(recording);
    const auto target = f.target("new-project", "Variation.adi");
    const auto originalTake = f.folder / recording;
    const auto saved = f.store->saveAs(target);
    check(saved.ok, "Save As switches an already named song to a new project folder");
    if (!saved.ok) return;

    const auto resolvedTake = [&] {
        std::string error;
        return media::resolveMedia(media::projectFolder(*f.store), media::mediaRow(f.store->db(), 10), error);
    };
    const auto linked = resolvedTake();
    check(fs::exists(f.source) && !linked.empty() && fs::equivalent(linked, originalTake),
          "named Save As preserves the original project and keeps referencing its audio");

    const auto hidden = f.temp.path() / "renamed-original";
    fs::rename(f.folder, hidden);
    check(resolvedTake().empty(), "named Save As depends on the original media until it is collected");
    fs::rename(hidden, f.folder);

    const auto collected = f.store->collectAllAndSave();
    check(collected.ok, "Collect All and Save copies the media into the new project");
    if (!collected.ok) return;
    fs::rename(f.folder, hidden);
    const auto local = resolvedTake();
    const auto relative = local.lexically_normal().lexically_relative(media::projectFolder(*f.store));
    check(!local.empty() && !relative.empty() && *relative.begin() != "..",
          "the collected variation still resolves its own copy after the original project moves");
}
void refusalsAndMissingMedia() {
    Fixture f;const auto target=f.target("pending");
    {
        SQLite::Transaction transaction(f.store->db());
        check(!f.store->saveCopy(target).ok && !fs::exists(target),"Copy refuses an uncommitted transaction without a file");
    }
    {
        SQLite::Statement statement(f.store->db(),"SELECT name FROM project");
        check(!f.store->saveAs(target).ok && !fs::exists(target),"Save As refuses live SQL statements before creating a file");
    }
    check(!f.store->saveCopy(f.temp.path()/"absent/Copy.adi").ok,"Copy reports a missing destination directory");
    fs::remove(f.folder/"audio/tone.wav");
    check(f.store->saveCopy(target).ok,"a project with missing media can still be saved");
    {audio::WavWriter wave(f.folder/"audio/tone.wav",48000,1,audio::WavFormat::Float32);
        std::array<float,512> samples{};samples.fill(0.25f);wave.write(samples.data(),512);}
    StoreError error{};auto copy=Store::open(target,error);
    check(copy && resolves(*copy),"the missing file's original location is retained for later recovery");
    const auto bad=f.target("invalid");
    f.store->db().exec("UPDATE ops SET payload=X'FF' WHERE op_type='media.import'");
    check(!f.store->saveCopy(bad).ok && !fs::exists(bad),"unreadable captured media history fails without publishing a broken copy");
    check(fs::is_empty(bad.parent_path()),"failed saves remove their private staging directory");
}
void readOnlySource() {
    Fixture f;StoreError error{};auto source=Store::open(f.source,error,true);
    check(source && source->saveCopy(f.target("readonly-copy")).ok,"read-only projects can be copied");
    check(source && source->saveAs(f.target("readonly-save-as")).ok && !source->readOnly(),
          "Save As makes an editable project from a supported read-only source");
}
void historyBranches() {
    Fixture f;auto& s=*f.store;
    fs::copy_file(f.folder/"audio/tone.wav",f.folder/"moved.wav");
    check(media::relinkMedia(s,10,"moved.wav").ok,"history relink fixture");
    History history(s);
    check(history.undo().ok,"relink can be undone before forking");
    OpRequest rename;rename.opType="project.setName";rename.payload={{"name","Fork"}};
    check(OpJournal(s).commit(rename).ok,"fork from the imported project");
    std::int64_t firstBranch=0;for(const auto& branch:history.branches())if(!branch.isCurrent)firstBranch=branch.id;
    const auto target=f.target("branched");check(s.saveCopy(target).ok,"copy includes inactive branches");
    StoreError error{};auto copy=Store::open(target,error);if(!copy)return;
    History copied(*copy);
    check(copied.switchToBranch(firstBranch).ok && resolves(*copy) && media::mediaRow(copy->db(),10).at("rel_path").get<std::string>().ends_with("moved.wav"),"switching to the old branch replays relocated relink paths");
    check(copied.undo().ok && resolves(*copy),"relink inverse paths are relocated too");
    check(copied.redo().ok,"restore relink for unlink capture");
    OpRequest detach;detach.opType="clip.detachAudio";detach.payload={{"id",5}};
    OpRequest unlink;unlink.opType="media.unlink";unlink.payload={{"id",10}};
    check(OpJournal(*copy).commit(std::vector<OpRequest>{detach,unlink}).ok,"capture an absent media row in unlink's inverse");
    const auto third=f.target("third");check(copy->saveCopy(third).ok,"a relocated copy can itself be copied");
    auto final=Store::open(third,error);
    check(final && History(*final).undo().ok && resolves(*final),"unlink's captured media row resolves after two relocations");
}
void symlinkedDestination() {
    // SQLite resolves the symlinks in a path it opens and projectFolder() reads that
    // spelling: macOS's temporary folder /var/... opens as /private/var/... A copy
    // saved through a symlink to a folder at another depth must count its relative
    // media paths from the folder it opens in, not from the caller's spelling.
    Fixture f;const auto real=f.temp.path()/"nested/real";fs::create_directories(real);
    std::error_code linked;fs::create_directory_symlink(real,f.temp.path()/"link",linked);
    if(linked){
#if defined(_WIN32)
        // Windows grants symlinks only to elevated or Developer Mode sessions.
        std::printf("SKIP symlinked destination checks: %s\n",linked.message().c_str());
#else
        // Unix always grants one here: a refusal fails rather than drops the guard.
        check(false,"the test folder takes a directory symlink");
#endif
        return;
    }
    const auto target=f.temp.path()/"link/Copy.adi";
    check(f.store->saveCopy(target).ok,"Copy publishes through a symlinked destination folder");
    StoreError error{};auto copy=Store::open(target,error);
    check(copy && resolves(*copy),"a copy saved through a symlink resolves media from the folder it opens in");
    if(!copy)return;
    History history(*copy);
    check(history.undo().ok && history.redo().ok && resolves(*copy),"its captured import paths resolve there too");
}
void checkpointRelocation() {
    Fixture f;History history(*f.store);UndoRetentionPolicy policy;policy.maxTransactions=2;
    check(history.setUndoRetentionPolicy(policy).ok,"checkpoint relocation policy");
    for(const char* name:{"A","B","C"}){OpRequest edit;edit.opType="project.setName";edit.payload={{"name",name}};if(!OpJournal(*f.store).commit(edit).ok)throw std::runtime_error("checkpoint edit");}
    const auto hash=f.store->db().execAndGet("SELECT state_hash FROM history_checkpoint_content WHERE id=1").getString();
    const auto copyPath=f.target("checkpoint-copy");check(f.store->saveCopy(copyPath).ok,"copy a compacted project with checkpoint media");
    StoreError error{};auto copy=Store::open(copyPath,error);check(copy!=nullptr,"compacted copy reopens");if(!copy)return;
    auto replay=Store::create(copyPath.parent_path()/"Replay.adi",error);
    if(!replay)throw std::runtime_error("checkpoint replay fixture (Replay.adi): "+std::string(toString(error)));
    check(History::restoreReplayBase(*copy,*replay).ok && resolves(*replay),"copied legacy checkpoint resolves media from its relocated folder");
    const auto copiedHash=copy->db().execAndGet("SELECT state_hash FROM history_checkpoint_content WHERE id=1").getString();
    check(copiedHash!=hash && f.store->db().execAndGet("SELECT state_hash FROM history_checkpoint_content WHERE id=1").getString()==hash,"copy refreshes its checkpoint content identity without changing source content");
    auto cached=Store::create(copyPath.parent_path()/"Cached.adi",error);
    if(!cached)throw std::runtime_error("cached replay fixture (Cached.adi): "+std::string(toString(error)));
    {SQLite::Transaction transaction(cached->db());history_detail::installCheckpoint(*cached,history_detail::readCheckpointContent(copy->db(),copiedHash));transaction.commit();}
    check(resolves(*cached) && digestProject(*cached).text==digestProject(*replay).text,"content-addressed checkpoint and compatibility image relocate identically");
}
void unknownMajorAndReaders() {
    Fixture f;f.store->db().exec("PRAGMA busy_timeout=0");
    {
        SQLite::Database reader(media::pathUtf8(f.source),SQLite::OPEN_READONLY);
        reader.exec("BEGIN");(void)reader.execAndGet("SELECT count(*) FROM tracks").getInt();
        const auto result=f.store->saveAs(f.target("busy-reader"));
        check(result.ok && !result.warning.empty(),"a held reader reports old-WAL cleanup as a warning after a successful switch");
        check(reader.execAndGet("SELECT count(*) FROM clips").getInt()==1,"the held reader's original data remains intact");
        reader.exec("ROLLBACK");
    }
    const auto future=f.target("future-source");
    {
        StoreError error{};auto s=Store::create(future,error);
        // DF-E12 (c): a refused create is a failed check, not a null dereference.
        check(s!=nullptr,"the future-major fixture is created");
        if(s)s->db().exec("PRAGMA user_version=2000");
    }
    StoreError error{};auto newer=Store::open(future,error);
    check(newer && error==StoreError::SchemaTooNew && newer->readOnly(),"unknown major fixture opens read-only");
    const auto target=f.target("future-copy");
    const auto copied=newer->saveCopy(target);
    check(copied.ok && !copied.warning.empty(),"unknown major supports a verbatim copy without interpreting its history");
    auto reopened=Store::open(target,error);
    check(reopened && reopened->schemaMajor()==2 && reopened->readOnly(),"the future copy keeps its version and read-only policy");
    const auto refused=f.target("future-save-as");
    check(!newer->saveAs(refused).ok && !fs::exists(refused),"Save As cannot turn an unsupported major into an editable project");
}

void stagedFileSaves() {
    Fixture fixture;
    auto& store = *fixture.store;
    store.db().exec("CREATE TABLE save_pages(bytes BLOB); INSERT INTO save_pages VALUES(zeroblob(2097152));");

    const auto copyPath = fixture.target("staged-copy");
    StoreFileSave copyTask;
    const auto pinned = store.prepareFileSave(copyPath, StoreSaveKind::copy, copyTask);
    check(pinned.ok, "prepare a copy of the current WAL snapshot");
    if (!pinned.ok) return;
    store.db().exec("UPDATE project SET name='Edited after pin' WHERE id=1");
    std::vector<StoreSaveProgress> progress;
    const auto written = std::async(std::launch::async, [&] {
        return copyTask.write({}, [&](StoreSaveProgress step) { progress.push_back(step); });
    }).get();
    check(written.ok && !copyTask.cancelled(), "file worker writes the pinned project");
    if (!written.ok) return;
    const auto complete = std::find_if(progress.begin(), progress.end(), [](const auto& step) {
        return step.phase == StoreSavePhase::copying && step.pagesCopied == step.totalPages;
    });
    check(!progress.empty() && progress.front().phase == StoreSavePhase::copying &&
          progress.front().pagesCopied > 0 && progress.front().pagesCopied < progress.front().totalPages &&
          complete != progress.end() && std::distance(complete, progress.end()) == 3 &&
          std::next(complete)->phase == StoreSavePhase::relocating && progress.back().phase == StoreSavePhase::publishing &&
          progress.back().pagesCopied == complete->totalPages && progress.back().totalPages == complete->totalPages,
          "worker reports chunked page progress before relocation and publication");
    StoreError error{};
    auto copy = Store::open(copyPath, error, true);
    check(copy && copy->db().execAndGet("SELECT name FROM project").getString() == "Original" &&
          copy->db().execAndGet("SELECT count(*) FROM audio_clips").getInt() == 1 && resolves(*copy) &&
          store.db().execAndGet("SELECT name FROM project").getString() == "Edited after pin",
          "copy contains committed WAL edits at invocation and rebases media while the owner stays current");

    const auto cancelPath = fixture.target("cancelled");
    StoreFileSave cancelTask;
    check(store.prepareFileSave(cancelPath, StoreSaveKind::copy, cancelTask).ok, "pin a cancelable copy");
    std::stop_source stop;
    const auto cancelled = std::async(std::launch::async, [&] {
        return cancelTask.write(stop.get_token(), [&](StoreSaveProgress step) {
            if (step.phase == StoreSavePhase::copying) stop.request_stop();
        });
    }).get();
    check(!cancelled.ok && cancelTask.cancelled() && fs::is_empty(cancelPath.parent_path()),
          "cancelling between backup chunks discards the stage without publishing");

    const auto stalePath = fixture.target("stale-save-as");
    StoreFileSave staleTask;
    const auto source = appliedSource(store);
    check(store.prepareFileSave(stalePath, StoreSaveKind::saveAs, staleTask).ok, "pin a prospective Save As");
    const auto staleCopy = std::async(std::launch::async, [&] { return staleTask.write(); }).get();
    check(staleCopy.ok, "a worker publishes a complete prospective Save As");
    {
        SQLite::Database other(media::pathUtf8(fixture.source), SQLite::OPEN_READWRITE);
        other.exec("UPDATE project SET name='External edit' WHERE id=1");
    }
    const auto stale = store.adoptFileSave(staleTask);
    auto frozen = Store::open(stalePath, error, true);
    check(!stale.ok && stale.error.find("original project changed") != std::string::npos && source.alive() &&
          store.path() == fixture.source && store.db().execAndGet("SELECT name FROM project").getString() == "External edit" &&
          frozen && frozen->db().execAndGet("SELECT name FROM project").getString() == "Edited after pin",
          "Save As refuses to abandon a newer external commit and keeps the completed copy");

    auto readonly = Store::open(fixture.source, error, true);
    check(readonly != nullptr, "readonly source opens for asynchronous Save As");
    if (!readonly) return;
    const auto savedPath = fixture.target("readonly-staged");
    StoreFileSave saveTask;
    const auto readonlySource = appliedSource(*readonly);
    check(readonly->prepareFileSave(savedPath, StoreSaveKind::saveAs, saveTask).ok, "pin readonly Save As");
    const auto ready = std::async(std::launch::async, [&] { return saveTask.write(); }).get();
    check(ready.ok && readonly->adoptFileSave(saveTask).ok && !readonly->readOnly() && readonly->path() == savedPath &&
          appliedSource(*readonly).isReplacementOf(readonlySource) && !readonlySource.alive() && resolves(*readonly),
          "owner adopts the exact readonly snapshot as a writable successor");
    const auto folded = std::async(std::launch::async, [&] { return saveTask.closeReplacedProject(); }).get();
    check(folded.empty(), "worker closes the detached readonly source");

    const auto busyPath = fixture.target("busy-staged");
    StoreFileSave busyTask;
    check(store.prepareFileSave(busyPath, StoreSaveKind::saveAs, busyTask).ok, "pin a copy before a competing writer");
    const auto busyCopy = std::async(std::launch::async, [&] { return busyTask.write(); }).get();
    check(busyCopy.ok, "completed copy survives a competing writer at adoption");
    {
        SQLite::Database writer(media::pathUtf8(fixture.source), SQLite::OPEN_READWRITE);
        SQLite::Transaction gate(writer, SQLite::TransactionBehavior::IMMEDIATE);
        const auto start = std::chrono::steady_clock::now();
        const auto refused = store.adoptFileSave(busyTask);
        check(!refused.ok && std::chrono::steady_clock::now() - start < std::chrono::seconds(1) &&
              store.path() == fixture.source && store.db().execAndGet("PRAGMA busy_timeout").getInt() == 5000,
              "a busy external writer refuses promptly without replacing the active Store's busy timeout");
    }
    check(store.adoptFileSave(busyTask).ok && store.path() == busyPath,
          "the completed copy can be adopted once the competing writer releases without editing");
    const auto closed = std::async(std::launch::async, [&] { return busyTask.closeReplacedProject(); }).get();
    check(closed.empty(), "worker folds the old writable project after releasing the writer gate");
}
}
int main(){try{copiesAndSwitches();namedSaveAsKeepsMediaReferences();refusalsAndMissingMedia();readOnlySource();historyBranches();symlinkedDestination();checkpointRelocation();unknownMajorAndReaders();stagedFileSaves();}catch(const std::exception& e){check(false,e.what());}
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
