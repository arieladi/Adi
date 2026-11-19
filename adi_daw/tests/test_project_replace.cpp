// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/store.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/audio/io_audit.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace {
using namespace adi;
namespace fs=std::filesystem;
int checks=0,failures=0;
fs::path executable;
void check(bool value,const char* name){++checks;if(!value){++failures;std::printf("FAIL: %s\n",name);}}
void write(const fs::path& path,const std::string& bytes){std::ofstream out(path,std::ios::binary);out<<bytes;if(!out)throw std::runtime_error("write fixture");}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
std::unique_ptr<Store> project(const fs::path& path,const char* name){StoreError error{};auto result=Store::create(path,error);if(!result)throw std::runtime_error(toString(error));result->db().exec("UPDATE project SET name='"+std::string(name)+"'");return result;}
std::string name(Store& store){return store.db().execAndGet("SELECT name FROM project").getString();}
void stores(){
    test::TempDirectory temp{"project_replace","stores"};const auto source=temp.path()/"Source.adi",target=temp.path()/fs::path(u8"יעד.adi");
    auto active=project(source,"Source");{auto old=project(target,"Old");}
    auto bytes=read(target);const auto client=active->clientId();
    check(!active->saveCopy(target).ok && read(target)==bytes,"default Copy refuses existing bytes");
    check(!active->saveAs(target).ok && active->path()==source && read(target)==bytes,"default Save As preserves both files");
    check(!active->saveCopy(source,StoreFileCollision::ReplaceConfirmed).ok && name(*active)=="Source","confirmed Copy refuses active source");
    const auto hard=temp.path()/"Alias.adi";fs::create_hard_link(source,hard);
    check(!active->saveAs(hard,StoreFileCollision::ReplaceConfirmed).ok && active->path()==source,"Save As refuses a hardlink to active source");
    {
        StoreError error{};auto reader=Store::open(target,error,true);
        check(reader && !active->saveCopy(target,StoreFileCollision::ReplaceConfirmed).ok && read(target)==bytes,"read-only Store lease prevents replacement");
    }
    {
        StoreError error{};auto writer=Store::open(target,error);
        check(writer && !active->saveAs(target,StoreFileCollision::ReplaceConfirmed).ok && active->path()==source,"live writer prevents Save As switch");
    }
    bytes=read(target); // Opening a writable SQLite connection legitimately changes header counters.
    for(const auto* suffix:{"-wal","-shm","-journal"}){
        auto sidecar=target;sidecar+=suffix;write(sidecar,"recovery sentinel");
        check(!active->saveCopy(target,StoreFileCollision::ReplaceConfirmed).ok && read(target)==bytes && read(sidecar)=="recovery sentinel","recovery sidecar refuses replacement without cleanup");
        fs::remove(sidecar);
    }
    check(!active->saveCopy(target,static_cast<StoreFileCollision>(99)).ok && read(target)==bytes,"unknown collision mode refuses replacement");
    check(active->saveCopy(target,StoreFileCollision::ReplaceConfirmed).ok && active->path()==source && name(*active)=="Source","confirmed Copy replaces closed target and keeps source active");
    {StoreError error{};auto copy=Store::open(target,error);check(copy && name(*copy)=="Source","replacement opens as complete source snapshot");}
    OpJournal journal(*active);History history(*active);OpRequest rename;rename.opType="project.setName";rename.payload={{"name","Edited"}};
    check(journal.commit(rename).ok,"history fixture before replacement Save As");
    check(active->saveAs(target,StoreFileCollision::ReplaceConfirmed).ok && active->path()==target && active->clientId()==client,"confirmed Save As switches path and retains client identity");
    check(history.undo().ok && name(*active)=="Source" && history.redo().ok && name(*active)=="Edited","existing journal/history follows replacement file");
    {StoreError error{};auto original=Store::open(source,error,true);check(original && name(*original)=="Edited","new-file Undo and Redo leave original source alone");}
    check(!active->saveCopy(target,StoreFileCollision::ReplaceConfirmed).ok,"the new active lease protects the replacement file");
    {std::error_code error;auto probe=media::ProjectFileLease::openExclusive(target,error);check(!probe,"Save As transfers the native lease with the new database");probe=media::ProjectFileLease::openExclusive(source,error);check(probe!=nullptr,"Save As releases the old database lease after close");}
    {auto closed=project(temp.path()/"Readonly.adi","Readonly");closed.reset();StoreError error{};auto readonly=Store::open(temp.path()/"Readonly.adi",error,true);check(readonly && readonly->saveCopy(temp.path()/"MemoryTarget.adi").ok,"readonly replacement fixture");check(readonly && readonly->saveAs(temp.path()/"MemoryTarget.adi",StoreFileCollision::ReplaceConfirmed).ok && !readonly->readOnly(),"confirmed replacement Save As promotes a supported readonly source");}
    StoreError error{};auto memory=Store::create(":memory:",error);check(memory!=nullptr,"in-memory project creation remains supported");
    auto temporary=Store::create({},error);check(temporary!=nullptr,"temporary SQLite project remains supported");
}
std::error_code fail(const fs::path&){return std::make_error_code(std::errc::io_error);}
std::error_code refuse(const fs::path&,const fs::path&){return std::make_error_code(std::errc::permission_denied);}
std::vector<unsigned> waits;unsigned renames=0;bool eventually=false;
bool retry(const std::error_code&){return true;}
void wait(unsigned ms){waits.push_back(ms);}
std::error_code delayed(const fs::path& source,const fs::path& target){++renames;if(eventually && renames==3)return media::renameReplacing(source,target);return std::make_error_code(std::errc::permission_denied);}
void publication(){
    test::TempDirectory temp{"project_replace","publish"};const auto source=temp.path()/"Stage",target=temp.path()/"Final";
    write(source,"new complete bytes");write(target,"old complete bytes");
    media::ReplaceHooks hooks;hooks.syncFile=fail;
    check(media::replaceFile(source,target,hooks).status==media::PublishStatus::failed && read(target)=="old complete bytes" && fs::exists(source),"prepublication sync failure preserves old file and stage");
    hooks={};hooks.rename=refuse;
    check(media::replaceFile(source,target,hooks).status==media::PublishStatus::failed && read(target)=="old complete bytes","rename failure preserves old bytes");
    hooks={};hooks.validateTarget=fail;
    check(media::replaceFile(source,target,hooks).status==media::PublishStatus::failed && read(target)=="old complete bytes","validation under lease preserves old bytes");
    std::error_code error;auto lease=media::ProjectFileLease::openShared(target,error);
    check(lease && media::replaceFile(source,target).status==media::PublishStatus::failed && read(target)=="old complete bytes","live destination lease refuses native replacement");lease.reset();
    lease=media::ProjectFileLease::openShared(source,error);hooks={};hooks.syncFile=fail;
    const auto blocked=media::replaceFile(source,target,hooks);
    check(lease && blocked.error.find("staging file is open")!=std::string::npos,"stage lease is acquired before extra sync descriptor opens");lease.reset();
    hooks={};hooks.rename=delayed;hooks.retryable=retry;hooks.retryWait=wait;
    waits.clear();renames=0;eventually=false;
    check(media::replaceFile(source,target,hooks).status==media::PublishStatus::failed && renames==8 && waits==std::vector<unsigned>({10,20,40,80,160,200,200}) && read(target)=="old complete bytes","transient rename retries have a fixed budget and preserve old bytes on exhaustion");
    waits.clear();renames=0;eventually=true;hooks.syncDirectory=fail;
    const auto published=media::replaceFile(source,target,hooks);
    check(published.status==media::PublishStatus::published && published.identity && !published.durabilityConfirmed && !published.warning.empty() && read(target)=="new complete bytes" && !fs::exists(source) && renames==3 && waits==std::vector<unsigned>({10,20}),"post-rename sync failure reports published bytes with warning, never unchanged failure");
    write(source,"second complete bytes");
    const auto complete=media::replaceFile(source,target);
    check(complete.status==media::PublishStatus::published && complete.identity && read(target)=="second complete bytes","native replacement publishes whole staged file");
    write(source,"missing target bytes");fs::remove(target);
    check(media::replaceFile(source,target).status==media::PublishStatus::published && read(target)=="missing target bytes","confirmed replacement can publish an absent endpoint");
    write(source,"atomic only");fs::remove(target);
    check(media::publishFile(source,target,{refuse},media::PublishPolicy::AtomicOnly).status==media::PublishStatus::failed && !fs::exists(target) && fs::exists(source),"authoritative no-clobber path never falls back to a visible copy");
    audio::onAudioThread=true;const auto rt=media::replaceFile(source,target);std::error_code denied;auto refusedLease=media::ProjectFileLease::openShared(source,denied);audio::onAudioThread=false;
    check(rt.status==media::PublishStatus::failed && !refusedLease && !fs::exists(target),"publication and lease acquisition refuse the audio thread");
}
#if defined(_WIN32)
struct PublishRetryFixture {
    fs::path source,target,parked;
    std::vector<unsigned> delays;
    unsigned syncCalls=0,renameCalls=0,syncFailures=0,renameFailures=0;
    std::error_code error{ERROR_SHARING_VIOLATION,std::system_category()};
    enum class Change { none, file, directory, otherWriter } change=Change::none;
    HANDLE held=INVALID_HANDLE_VALUE;
    bool releaseOnWait=false;
    explicit PublishRetryFixture(const fs::path& folder):source(folder/"Stage"),target(folder/"Final"),parked(folder/"Parked") {
        fs::create_directory(folder);write(source,"whole staged payload");
    }
    ~PublishRetryFixture(){if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);}
};
PublishRetryFixture* publishRetry=nullptr;
std::error_code testExclusiveRename(const fs::path& source,const fs::path& target){
    if(MoveFileExW(source.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))return {};
    const auto error=GetLastError();
    if(error==ERROR_ALREADY_EXISTS || error==ERROR_FILE_EXISTS)return std::make_error_code(std::errc::file_exists);
    return {static_cast<int>(error),std::system_category()};
}
std::error_code controlledPublishSync(const fs::path&){
    return ++publishRetry->syncCalls<=publishRetry->syncFailures?publishRetry->error:std::error_code{};
}
std::error_code controlledPublishRename(const fs::path& source,const fs::path& target){
    if(++publishRetry->renameCalls<=publishRetry->renameFailures)return publishRetry->error;
    return testExclusiveRename(source,target);
}
void publishRetryWait(unsigned delay){
    auto& state=*publishRetry;state.delays.push_back(delay);
    if(state.releaseOnWait && state.held!=INVALID_HANDLE_VALUE){
        if(!CloseHandle(state.held))throw std::runtime_error("close retry fixture holder");
        state.held=INVALID_HANDLE_VALUE;
    }
    if(state.change==PublishRetryFixture::Change::otherWriter)write(state.target,"other writer");
    else if(state.change!=PublishRetryFixture::Change::none){
        fs::rename(state.source,state.parked);
        if(state.change==PublishRetryFixture::Change::file)write(state.source,"different file");
        else fs::create_directory(state.source);
    }
    state.change=PublishRetryFixture::Change::none;
}
media::FileIdentity fixtureIdentity(const fs::path& path){
    std::error_code error;auto lease=media::ProjectFileLease::openShared(path,error);
    if(!lease)throw std::runtime_error("fixture identity: "+error.message());
    return lease->identity();
}
void windowsPublicationRetries(){
    test::TempDirectory temp{"project_replace","windows_retry"};
    unsigned next=0;
    const auto folder=[&]{return temp.path()/std::to_string(++next);};
    media::PublishHooks hooks;hooks.rename=controlledPublishRename;
    hooks.syncFile=controlledPublishSync;hooks.retryWait=publishRetryWait;
    const std::vector<unsigned> budget{10,20,40,80,160,200,200};
    for(const int code:{ERROR_ACCESS_DENIED,ERROR_SHARING_VIOLATION,ERROR_LOCK_VIOLATION}){
        PublishRetryFixture state(folder());publishRetry=&state;state.renameFailures=2;
        state.error={code,std::system_category()};const auto identity=fixtureIdentity(state.source);
        const auto result=media::publishFile(state.source,state.target,hooks,media::PublishPolicy::AtomicOnly);
        check(result.status==media::PublishStatus::published && result.identity==identity && fixtureIdentity(state.target)==identity && !result.usedFallback && !fs::exists(state.source) && read(state.target)=="whole staged payload","each Windows transient rename error publishes complete bytes with original identity");
        check(state.syncCalls==1 && state.renameCalls==3 && state.delays==std::vector<unsigned>({10,20}),"rename retries do not repeat successful stage sync");
    }
    for(const int code:{ERROR_ACCESS_DENIED,ERROR_SHARING_VIOLATION,ERROR_LOCK_VIOLATION}){
        PublishRetryFixture state(folder());publishRetry=&state;state.syncFailures=2;
        state.error={code,std::system_category()};
        const auto result=media::publishFile(state.source,state.target,hooks,media::PublishPolicy::AtomicOnly);
        check(result.status==media::PublishStatus::published && state.syncCalls==3 && state.renameCalls==1 && state.delays==std::vector<unsigned>({10,20}) && read(state.target)=="whole staged payload","each Windows transient sync error recovers before exclusive rename");
    }
    {
        PublishRetryFixture state(folder());publishRetry=&state;state.syncFailures=2;state.renameFailures=2;
        const auto result=media::publishFile(state.source,state.target,hooks,media::PublishPolicy::AtomicOnly);
        check(result.status==media::PublishStatus::published && state.syncCalls==3 && state.renameCalls==3 && state.delays==std::vector<unsigned>({10,20,40,80}),"sync and exclusive rename consume one shared retry budget");
    }
    for(const unsigned syncFailures:{0u,2u,100u}){
        PublishRetryFixture state(folder());publishRetry=&state;state.syncFailures=syncFailures;state.renameFailures=100;
        const auto result=media::publishFile(state.source,state.target,hooks,media::PublishPolicy::AtomicOnly);
        const auto syncCalls=syncFailures==100?8u:syncFailures+1;
        const auto renameCalls=syncFailures==100?0u:8u-syncFailures;
        check(result.status==media::PublishStatus::failed && !result.usedFallback && state.syncCalls==syncCalls && state.renameCalls==renameCalls && state.delays==budget && read(state.source)=="whole staged payload" && !fs::exists(state.target),"exhausted shared sync/rename budget retains stage and exposes no target");
    }
    for(const auto error:{std::error_code(ERROR_NOT_SAME_DEVICE,std::system_category()),std::error_code(ERROR_DISK_FULL,std::system_category()),std::make_error_code(std::errc::permission_denied),std::make_error_code(std::errc::file_exists)}){
        for(const bool sync:{false,true}){
            PublishRetryFixture state(folder());publishRetry=&state;state.error=error;
            state.syncFailures=sync?100u:0u;state.renameFailures=100;
            const auto result=media::publishFile(state.source,state.target,hooks,media::PublishPolicy::AtomicOnly);
            const auto expected=!sync && error==std::errc::file_exists?media::PublishStatus::exists:media::PublishStatus::failed;
            check(result.status==expected && !result.usedFallback && state.syncCalls==1 && state.renameCalls==(sync?0u:1u) && state.delays.empty() && fs::exists(state.source) && !fs::exists(state.target),"non-transient/native cross-volume/full/generic and exists errors never retry or copy");
        }
    }
    for(const bool duringSync:{false,true}){
        for(const auto change:{PublishRetryFixture::Change::file,PublishRetryFixture::Change::directory}){
            PublishRetryFixture state(folder());publishRetry=&state;state.change=change;
            state.syncFailures=duringSync?1u:0u;state.renameFailures=duringSync?0u:1u;
            const auto result=media::publishFile(state.source,state.target,hooks,media::PublishPolicy::AtomicOnly);
            check(result.status==media::PublishStatus::failed && state.syncCalls==1 && state.renameCalls==(duringSync?0u:1u) && state.delays==std::vector<unsigned>({10}) && read(state.parked)=="whole staged payload" && !fs::exists(state.target) && (change==PublishRetryFixture::Change::file?read(state.source)=="different file":fs::is_directory(state.source)),"changed stage identity/type during retry refuses without touching either file");
        }
    }
    {
        PublishRetryFixture state(folder());publishRetry=&state;state.renameFailures=1;state.change=PublishRetryFixture::Change::otherWriter;
        const auto result=media::publishFile(state.source,state.target,hooks,media::PublishPolicy::AtomicOnly);
        check(result.status==media::PublishStatus::exists && state.renameCalls==2 && state.delays==std::vector<unsigned>({10}) && read(state.target)=="other writer" && read(state.source)=="whole staged payload","exclusive retry preserves a destination claimed by another writer");
    }
    for(const DWORD sharing:{DWORD(FILE_SHARE_READ),DWORD(FILE_SHARE_READ|FILE_SHARE_WRITE)}){
        for(const bool release:{false,true}){
            PublishRetryFixture state(folder());publishRetry=&state;state.releaseOnWait=release;
            state.held=CreateFileW(state.source.c_str(),GENERIC_READ,sharing,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(state.held==INVALID_HANDLE_VALUE)throw std::runtime_error("open real retry holder");
            media::PublishHooks native;native.retryWait=publishRetryWait;
            const auto result=media::publishFile(state.source,state.target,native,media::PublishPolicy::AtomicOnly);
            if(release){
                check(result.status==media::PublishStatus::published && result.identity && !result.usedFallback && state.delays==std::vector<unsigned>({10}) && !fs::exists(state.source) && read(state.target)=="whole staged payload","real sync/rename sharing holder released inside retry window allows publication");
            }else{
                check(result.status==media::PublishStatus::failed && !result.usedFallback && state.delays==budget && !fs::exists(state.target) && read(state.source)=="whole staged payload","real permanent sharing holder exhausts bounded retries with original stage intact");
                CloseHandle(state.held);state.held=INVALID_HANDLE_VALUE;
                check(fs::remove(state.source) && !fs::exists(state.target),"stage owner can clean failed real-handle fixture after holder closes");
            }
        }
    }
    {
        PublishRetryFixture state(folder());publishRetry=&state;
        audio::onAudioThread=true;const auto result=media::publishFile(state.source,state.target,hooks,media::PublishPolicy::AtomicOnly);audio::onAudioThread=false;
        check(result.status==media::PublishStatus::failed && state.syncCalls==0 && state.renameCalls==0 && state.delays.empty() && fs::exists(state.source) && !fs::exists(state.target),"audio thread refuses publication before hooks, I/O or waits");
    }
    publishRetry=nullptr;
}
#endif
void publicationRetries(){
    test::TempDirectory temp{"project_replace","no_native_retry"};const auto source=temp.path()/"Stage",target=temp.path()/"Final";
    write(source,"stage");media::PublishHooks hooks;hooks.rename=refuse;hooks.retryWait=wait;waits.clear();
    check(media::publishFile(source,target,hooks,media::PublishPolicy::AtomicOnly).status==media::PublishStatus::failed && waits.empty() && read(source)=="stage" && !fs::exists(target),"generic rename refusal never sleeps or publishes on any host");
    hooks.syncDirectory=fail;
    const auto copy=media::publishFile(source,target,hooks,media::PublishPolicy::VerifiedCopyFallback);
    check(copy.status==media::PublishStatus::published && copy.usedFallback && copy.identity && waits.empty() && !fs::exists(source) && read(target)=="stage","explicit verified-copy policy retains its prior private-copy publication behavior");
    check(!copy.durabilityConfirmed && !copy.warning.empty(),"verified copy reports the post-publication directory sync failure");
    write(source,"next stage");hooks.rename=nullptr;waits.clear();
    check(media::publishFile(source,target,hooks,media::PublishPolicy::AtomicOnly).status==media::PublishStatus::exists && waits.empty() && read(source)=="next stage" && read(target)=="stage","actual existing destination returns exists immediately without a retry");
#if defined(_WIN32)
    windowsPublicationRetries();
#endif
}
#if !defined(_WIN32)
bool childStatus(pid_t child,int expected){int status=0;return child>0 && ::waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==expected;}
int writerResult(const fs::path& path){sqlite3* db=nullptr;if(sqlite3_open(media::pathUtf8(path).c_str(),&db)!=SQLITE_OK)return 99;const int result=sqlite3_exec(db,"BEGIN IMMEDIATE; UPDATE project SET name='Child'; COMMIT",nullptr,nullptr,nullptr);sqlite3_close(db);return result==SQLITE_BUSY?1:result==SQLITE_OK?0:99;}
pid_t writerProcess(const fs::path& path){const auto child=::fork();if(child==0){const auto filename=media::pathUtf8(path);const auto program=media::pathUtf8(executable);::execl(program.c_str(),program.c_str(),"--writer",filename.c_str(),static_cast<char*>(nullptr));::_exit(99);}return child;}
std::error_code crashAfterRename(const fs::path&){::_exit(23);}
void processes(){
    test::TempDirectory temp{"project_replace","processes"};const auto target=temp.path()/"Target.adi",source=temp.path()/"Source.adi";
    {auto store=project(target,"Old");store->close();}
    auto active=project(source,"Source");
    SQLite::Database reader(media::pathUtf8(target),SQLite::OPEN_READONLY);reader.exec("BEGIN");(void)reader.execAndGet("SELECT name FROM project");
    {StoreError error{};auto second=Store::open(target,error,true);check(second!=nullptr,"second Store opens beside raw SQLite reader");}
    // A retained SQLite reader is outside the managed lease protocol. Closing
    // an extra native DB fd must not silently drop its POSIX record locks.
    auto child=writerProcess(target);
    check(childStatus(child,1),"opening/closing Store leases preserves raw SQLite reader's process locks");
    std::error_code error;auto lease=media::ProjectFileLease::openShared(target,error);
    check(!active->saveCopy(target,StoreFileCollision::ReplaceConfirmed).ok,"failed replacement with a shared lease leaves raw reader in place");
    lease.reset();child=writerProcess(target);
    check(childStatus(child,1),"failed replacement and lease close preserve SQLite's process locks");reader.exec("ROLLBACK");
    child=writerProcess(target);check(childStatus(child,0),"writer progresses after actual raw read transaction ends");
    // Cross-process lease exclusion and release are tested through pipes, with
    // no time-based sleep or timeout expansion.
    int ready[2]{},release[2]{};if(::pipe(ready)!=0 || ::pipe(release)!=0)throw std::runtime_error("pipe");
    child=::fork();if(child==0){::close(ready[0]);::close(release[1]);std::error_code ec;auto held=media::ProjectFileLease::openShared(target,ec);char marker=held?'Y':'N';(void)::write(ready[1],&marker,1);(void)::read(release[0],&marker,1);::_exit(held?0:99);}
    ::close(ready[1]);::close(release[0]);char marker=0;const auto got=::read(ready[0],&marker,1);
    check(got==1 && marker=='Y' && !active->saveCopy(target,StoreFileCollision::ReplaceConfirmed).ok,"another process's lease refuses confirmed replacement");
    (void)::write(release[1],"X",1);::close(ready[0]);::close(release[1]);check(childStatus(child,0),"lease-holder process exits cleanly");
    check(active->saveCopy(target,StoreFileCollision::ReplaceConfirmed).ok,"replacement proceeds after other process exits");active.reset();
    const auto staged=temp.path()/"Stage";write(staged,"crash complete bytes");write(target,"previous complete bytes");
    child=::fork();if(child==0){media::ReplaceHooks hooks;hooks.syncDirectory=crashAfterRename;(void)media::replaceFile(staged,target,hooks);::_exit(99);}
    check(childStatus(child,23) && read(target)=="crash complete bytes" && !fs::exists(staged),"process exit immediately after rename leaves the complete new file");
}
#endif
}
int main([[maybe_unused]] int argc,char** argv){
#if !defined(_WIN32)
if(argc==3 && std::string(argv[1])=="--writer")return writerResult(media::pathFromUtf8(argv[2]));
#endif
executable=fs::absolute(argv[0]);try{stores();publication();publicationRetries();
#if !defined(_WIN32)
processes();
#endif
}catch(const std::exception& error){check(false,error.what());}std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
