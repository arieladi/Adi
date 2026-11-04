// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/digest.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <stdexcept>
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const std::string& text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text.c_str());}}
OpRequest role(std::int64_t id,Payload value){OpRequest request;request.opType="track.setRole";request.payload={{"id",id},{"role",std::move(value)}};return request;}
void run() {
    test::TempDirectory directory{"track_roles","project"};StoreError error{};auto store=Store::create(directory.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','One'),(2,'group','Two'),(3,'master','Main'),(4,'marker','Markers'),(1099511627776,'instrument','Wide')");
    auto frame=SnapshotBuilder::fromStore(*store);check(frame->tracks.size()==5 && !frame->findTrack(1)->role,"older schema snapshot has no assigned role");
    const auto original=digestProject(*store).text;const auto unavailable=OpJournal(*store).commit(role(1,"reference"));
    check(!unavailable.ok && unavailable.error.find("migration")!=std::string::npos && digestProject(*store).text==original,"missing win-owned format refuses atomically without inventing a table/column");
    // Only a synthetic reader/op fixture: this is not the format migration.
    store->db().exec("ALTER TABLE tracks ADD COLUMN role TEXT");
    auto previous=SnapshotBuilder::fromStore(*store,*frame);check(previous->findTrack(1)==frame->findTrack(1),"adding nullable native column leaves unchanged track nodes shared");
    for(const char* value:{"kick","bass","vocal","reference"}) {
        const auto committed=OpJournal(*store).commit(role(1,value));check(committed.ok,std::string("native role assignment: ")+value+": "+committed.error);
        frame=SnapshotBuilder::fromStore(*store,*previous);
        check(frame->findTrack(1)->role==value && frame->findTrack(1)!=previous->findTrack(1) && frame->findTrack(2)==previous->findTrack(2),"role participates in structural sharing without replacing other tracks");
        previous=std::move(frame);
    }
    check(!OpJournal(*store).commit(role(2,"reference")).ok,"second reference refuses instead of silently redirecting another track");
    const std::array<OpRequest,2> move={role(1,nullptr),role(2,"reference")};
    check(OpJournal(*store).commit(move).ok,"explicit two-id batch moves the reference in one undo step");
    check(History(*store).undo().ok && SnapshotBuilder::fromStore(*store)->findTrack(1)->role=="reference" && !SnapshotBuilder::fromStore(*store)->findTrack(2)->role,"undo restores nullable roles and reference identity");
    check(History(*store).redo().ok && SnapshotBuilder::fromStore(*store)->findTrack(2)->role=="reference","redo reassigns reference deterministically");
    OpRequest clear;clear.opType="track.setRole";clear.payload={{"id",2}};
    check(OpJournal(*store).commit(clear).ok && !SnapshotBuilder::fromStore(*store)->findTrack(2)->role,"omitted optional role clears to SQL NULL");
    const auto beforeBad=digestProject(*store).text;
    for(const auto& request:{role(1,"drums"),role(3,"kick"),role(4,"vocal"),role(99,"bass"),role(1,5)})
        check(!OpJournal(*store).commit(request).ok && digestProject(*store).text==beforeBad,"invalid role, track kind/id or payload leaves no mutation");
    check(OpJournal(*store).commit(role(1099511627776LL,"kick")).ok && SnapshotBuilder::fromStore(*store)->findTrack(1099511627776LL)->role=="kick","role targets retain 64-bit ids");
    store->db().exec("UPDATE tracks SET role='future' WHERE id=1");frame=SnapshotBuilder::fromStore(*store,*previous);
    check(frame->findTrack(1)->role=="future","snapshot retains an unknown future role for display and sharing");
    const auto future=digestProject(*store).text;
    check(!OpJournal(*store).commit(role(1,nullptr)).ok && digestProject(*store).text==future,"unknown stored future role is not overwritten with a lossy inverse");
    const auto path=store->path();store.reset();store=Store::open(path,error);
    check(store && History(*store).undo().ok && !SnapshotBuilder::fromStore(*store)->findTrack(1099511627776LL)->role,"role inverse survives CBOR and reopen");
    const auto* descriptor=OpRegistry::instance().find("track.setRole");
    check(descriptor && descriptor->engineImpact==EngineImpact::GraphRebuild && !descriptor->coalescable,"role op rebuilds routing for the independent N23 cue backend");
}
void replay() {
    test::TempDirectory directory{"track_roles","replay"};StoreError error{};
    auto first=Store::createEmpty(directory.path()/"one.adi",error);auto second=Store::createEmpty(directory.path()/"two.adi",error);
    if(!first || !second)throw std::runtime_error(toString(error));
    for(auto* project:{first.get(),second.get()})project->db().exec("ALTER TABLE tracks ADD COLUMN role TEXT;INSERT INTO tracks(id,kind,name) VALUES(1,'audio','One'),(2,'group','Two')");
    first->db().exec("INSERT INTO session_state VALUES('selection','1')");second->db().exec("INSERT INTO session_state VALUES('selection','other')");
    for(const auto& request:{role(1,"reference"),role(1,nullptr),role(2,"reference")})
        check(OpJournal(*first).commit(request).ok && OpJournal(*second).commit(request).ok,"literal role log replays with different ambient selection");
    check(digestProject(*first).text==digestProject(*second).text && History(*first).undo().ok && History(*second).undo().ok && digestProject(*first).text==digestProject(*second).text,"role replay and nullable undo produce identical project rows");
    check(History(*first).redo().ok && History(*second).redo().ok && digestProject(*first).text==digestProject(*second).text,"role replay redo is deterministic");
}
}
int main(){try{run();replay();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
