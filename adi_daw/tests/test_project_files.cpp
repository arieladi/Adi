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
void templates() {
    Fixture f;
    auto &source = *f.store;
    History history(source);
    UndoRetentionPolicy policy;
    policy.maxTransactions = 2;
    check(history.setUndoRetentionPolicy(policy).ok, "template source retention fixture");
    OpRequest edit;
    edit.opType = "project.setName";
    for (const auto *name : {"A", "B", "C"}) {
        edit.payload = {{"name", name}};
        if (!OpJournal(source).commit(edit).ok)
            throw std::runtime_error("template source edit");
    }
    check(history.undo().ok, "template source can leave a prior redo tip");
    edit.payload = {{"name", "Template State"}};
    check(OpJournal(source).commit(edit).ok && history.branches().size() >= 2,
          "template source has an inactive undo branch");
    source.db().exec("INSERT INTO session_lock VALUES(1,'original-host',12,'fixture',1,2);"
                     "INSERT INTO history_snapshots(id,name,op_seq,branch_id,created_utc) "
                     "SELECT 1,'Original snapshot',head_seq,id,0 FROM op_branches WHERE is_current=1;"
                     "INSERT INTO agent_requests(txn_id,request,created_utc) VALUES(1,'Original request',0);"
                     "UPDATE adi_meta SET value='source-uuid' WHERE key='project_uuid';");
    const auto sourceHistory = historyBytes(source);
    const auto sourceDigest = digestProject(source).text;
    const auto sourceHead = OpJournal(source).headSeq();
    const auto path = f.target("User Library/Templates", fs::path(u8"תבנית.adi"));
    const auto ordinary = f.target("ordinary");
    check(!source.isTemplate() && !source.createFromTemplate(ordinary).ok && !fs::exists(ordinary),
          "an ordinary project is refused as a template and publishes nothing");
    const auto saved = source.saveTemplate(path);
    check(saved.ok, saved.error.empty() ? "save committed project state as template" : saved.error.c_str());
    if (!saved.ok)
        return;
    check(!source.saveTemplate(path).ok && !source.saveTemplate(f.source).ok &&
              fs::equivalent(source.path(), f.source) && !source.isTemplate() &&
              sourceHistory == historyBytes(source) && sourceDigest == digestProject(source).text &&
              sourceHead == OpJournal(source).headSeq() &&
              source.db().execAndGet("SELECT count(*) FROM session_lock").getInt() == 1,
          "template publication keeps the active source and refuses a default collision");
    StoreError error{};
    auto stored = Store::open(path, error, true);
    check(stored && stored->readOnly() && stored->isTemplate(), "the library template opens with its marker");
    if (!stored)
        return;
    const auto templateUuid = stored->db().execAndGet(
        "SELECT value FROM adi_meta WHERE key='project_uuid'").getString();
    check(templateUuid.size() == 32 && templateUuid != "source-uuid" &&
              stored->db().execAndGet("SELECT count(*) FROM session_lock").getInt() == 0 &&
              stored->db().execAndGet("SELECT count(*) FROM ops WHERE op_type<>'history.root'").getInt() == 0 &&
              stored->db().execAndGet("SELECT count(*) FROM agent_requests").getInt() == 0 &&
              stored->db().execAndGet("SELECT count(*) FROM history_snapshots").getInt() == 0 &&
              stored->db().execAndGet("SELECT count(*) FROM op_branches").getInt() == 1 &&
              !History(*stored).canUndo() && !History(*stored).canRedo() && resolves(*stored),
          "the template keeps its state and media with a fresh independent root");
    const auto createdPath = f.target("sets/nested", "New.adi");
    const auto created = stored->createFromTemplate(createdPath);
    check(created.ok, created.error.empty() ? "create a Set from a read-only template" : created.error.c_str());
    check(!stored->createFromTemplate(createdPath).ok && stored->isTemplate(),
          "instantiating an existing pathname preserves its file and source template");
    auto project = Store::open(createdPath, error);
    check(project && !project->isTemplate() && resolves(*project), "the new Set is writable and resolves source audio");
    if (!project)
        return;
    check(project->db().execAndGet("SELECT value FROM adi_meta WHERE key='project_uuid'").getString() != templateUuid &&
              project->db().execAndGet("SELECT name FROM project").getString() == "Template State",
          "the resulting Set gets a new identity and the current template project state");
    const auto baseline = digestProject(*project).text;
    History newHistory(*project);
    edit.payload = {{"name", "First edit"}};
    check(!newHistory.canUndo() && OpJournal(*project).commit(edit).ok && newHistory.canUndo(),
          "the first edit is the only undoable step in a new Set");
    auto replay = Store::create(createdPath.parent_path() / "Replay.adi", error);
    if (!replay)
        throw std::runtime_error("cannot create template replay fixture");
    const auto replayBase = History::restoreReplayBase(*project, *replay);
    check(replayBase.ok && replayBase.retainedSeqs.size() == 1 && resolves(*replay) &&
              digestProject(*replay).text == baseline,
          "verifiable replay restores the exact rebased template root and one retained edit");
    check(newHistory.undo().ok && !newHistory.canUndo() && digestProject(*project).text == baseline &&
              newHistory.redo().ok && project->db().execAndGet("SELECT name FROM project").getString() == "First edit",
          "Undo returns to the template root and Redo restores the first edit");
    project.reset();
    project = Store::open(createdPath, error);
    check(project && History(*project).undo().ok && digestProject(*project).text == baseline,
          "the new baseline survives close, reopen and undo");
}
void olderTemplate() {
    Fixture f;
    const auto sourcePath = f.target("legacy", "Template.adi");
    check(f.store->saveTemplate(sourcePath).ok, "older template fixture is saved");
    {
        SQLite::Database old(media::pathUtf8(sourcePath), SQLite::OPEN_READWRITE);
        old.exec("DROP TABLE routing_send_taps;"
                 "UPDATE adi_meta SET value='22' WHERE key='schema_minor';"
                 "PRAGMA user_version=1022;");
    }
    StoreError error{};
    auto source = Store::open(sourcePath, error, true);
    check(source && source->isTemplate() && source->schemaMinor() == 22,
          "a supported older file can be read as a template");
    if (!source)
        return;
    const auto path = f.target("legacy-instance", "Instance.adi");
    const auto created = source->createFromTemplate(path);
    check(created.ok, created.error.empty() ? "upgrade the private copy during creation" : created.error.c_str());
    auto instance = Store::open(path, error);
    check(instance && instance->schemaMinor() == kSchemaMinor && !instance->upgradedFromMinor() &&
              instance->db().tableExists("routing_send_taps") && resolves(*instance) &&
              !History(*instance).canUndo(),
          "the delivered Set has the current schema and usable media and history");
    check(source->db().execAndGet("PRAGMA user_version").getInt() == 1022 && source->isTemplate(),
          "template creation leaves the original file at its exact older version");
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
    check(!newer->saveTemplate(refused).ok && !fs::exists(refused),
          "an unsupported schema cannot be rewritten as a template");
}
}
int main(){try{copiesAndSwitches();refusalsAndMissingMedia();readOnlySource();historyBranches();symlinkedDestination();checkpointRelocation();templates();olderTemplate();unknownMajorAndReaders();}catch(const std::exception& e){check(false,e.what());}
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
