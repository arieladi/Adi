// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/store.hpp"
#include "adi/ops.hpp"
#include "adi/history.hpp"
#include "adi/history_compaction.hpp"
#include "adi/digest.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/session.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <algorithm>
#include <cstdio>

namespace {
using namespace adi;
namespace fs=std::filesystem;
int checks=0,failures=0;
void check(bool good,const char* name){++checks;if(!good){++failures;std::printf("FAIL: %s\n",name);}}
struct Fixture {
    test::TempDirectory temp{"project_files","source"};
    std::unique_ptr<Store> store;
    fs::path source,folder;
    Fixture() {
        folder=temp.path()/"original";fs::create_directories(folder/"audio");
        source=folder/"Source.adi";StoreError error{};store=Store::createEmpty(source,error);
        if(!store)throw std::runtime_error("create fixture");
        store->db().exec("INSERT INTO project(id,name) VALUES(1,'Original');"
            "INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Audio'),(9,'master','Main');");
        {audio::WavWriter wave(folder/"audio/tone.wav",48000,1,audio::WavFormat::Float32);
            std::array<float,512> samples{};samples.fill(0.25f);wave.write(samples.data(),512);}
        OpRequest capture;const auto prepared=media::prepareImport(*store,"audio/tone.wav",10,1,capture);
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
    auto replay=Store::create(copyPath.parent_path()/"Replay.adi",error);if(!replay)throw std::runtime_error("checkpoint replay fixture");
    check(History::restoreReplayBase(*copy,*replay).ok && resolves(*replay),"copied legacy checkpoint resolves media from its relocated folder");
    const auto copiedHash=copy->db().execAndGet("SELECT state_hash FROM history_checkpoint_content WHERE id=1").getString();
    check(copiedHash!=hash && f.store->db().execAndGet("SELECT state_hash FROM history_checkpoint_content WHERE id=1").getString()==hash,"copy refreshes its checkpoint content identity without changing source content");
    auto cached=Store::create(copyPath.parent_path()/"Cached.adi",error);if(!cached)throw std::runtime_error("cached replay fixture");
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
    {StoreError error{};auto s=Store::create(future,error);s->db().exec("PRAGMA user_version=2000");}
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
}
int main(){try{copiesAndSwitches();refusalsAndMissingMedia();readOnlySource();historyBranches();symlinkedDestination();checkpointRelocation();unknownMajorAndReaders();}catch(const std::exception& e){check(false,e.what());}
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
