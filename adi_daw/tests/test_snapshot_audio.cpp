// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <stdexcept>
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
const ClipNode* clip(const Snapshot& frame,std::int64_t id){for(const auto& track:frame.tracks)for(const auto& node:track->clips)if(node->id==id)return node.get();return nullptr;}
void run() {
    test::TempDirectory directory{"snapshot_audio","project"};StoreError error{};auto store=Store::create(directory.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));auto& db=store->db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Audio');INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) VALUES(1,1,'audio','One',0,960),(2,1,'midi','Two',0,960);"
        "INSERT INTO media_files(id,hash_blake3,sample_rate,channels,frames) VALUES(1,'first',96000,2,1099511627776),(1099511627776,'wide',48000,1,1099511627777);"
        "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,1099511627700,72)");
    const auto original=SnapshotBuilder::fromStore(*store);auto frame=SnapshotBuilder::fromStore(*store,*original);const auto* source=&*clip(*frame,1)->audio;
    check(source->mediaId==1 && source->sourceStartFrames==1099511627700LL && source->sourceLengthFrames==72 && source->mediaInfo->sampleRate==96000 && source->mediaInfo->frames==1099511627776LL,"source frames and pool dimensions retain source rate and 64-bit width");
    check(!clip(*frame,2)->audio && clip(*frame,1)==clip(*original,1),"no attachment differs from an audio source; unchanged audio shares its node");
    const std::array<const char*,20> edits={
        "UPDATE audio_clips SET media_id=1099511627776 WHERE clip_id=1",
        "UPDATE audio_clips SET src_start_frames=1099511627701 WHERE clip_id=1",
        "UPDATE audio_clips SET src_len_frames=71 WHERE clip_id=1",
        "UPDATE audio_clips SET warp_enabled=1 WHERE clip_id=1",
        "UPDATE audio_clips SET warp_mode='beats' WHERE clip_id=1",
        "UPDATE audio_clips SET warp_markers=X'' WHERE clip_id=1",
        "UPDATE audio_clips SET warp_markers=X'00ff' WHERE clip_id=1",
        "UPDATE audio_clips SET transpose_semis=-3.5 WHERE clip_id=1",
        "UPDATE audio_clips SET formant_shift=1.25 WHERE clip_id=1",
        "UPDATE audio_clips SET reverse=1 WHERE clip_id=1",
        "UPDATE audio_clips SET channel_mode=3 WHERE clip_id=1",
        "UPDATE media_files SET hash_blake3='changed' WHERE id=1099511627776",
        "UPDATE media_files SET sample_rate=192000 WHERE id=1099511627776",
        "UPDATE media_files SET channels=4 WHERE id=1099511627776",
        "UPDATE media_files SET frames=1099511627788 WHERE id=1099511627776",
        "UPDATE media_files SET missing=1 WHERE id=1099511627776",
        "UPDATE media_files SET sample_rate=NULL WHERE id=1099511627776",
        "UPDATE media_files SET channels=NULL WHERE id=1099511627776",
        "UPDATE media_files SET frames=NULL WHERE id=1099511627776",
        "UPDATE audio_clips SET warp_markers=NULL WHERE clip_id=1"};
    for(const char* edit:edits){auto previous=std::move(frame);db.exec(edit);frame=SnapshotBuilder::fromStore(*store,*previous);check(clip(*frame,1)!=clip(*previous,1) && clip(*frame,2)==clip(*previous,2),"every source and pool identity field invalidates sharing without touching other clips");}
    check(clip(*original,1)->audio->mediaInfo->contentHash=="first" && !clip(*original,1)->audio->warpMarkers && !clip(*original,1)->audio->reverse,"old published audio metadata remains immutable");
    auto previous=std::move(frame);db.exec("UPDATE media_files SET peaks=X'00ff' WHERE id=1099511627776");frame=SnapshotBuilder::fromStore(*store,*previous);
    check(clip(*frame,1)==clip(*previous,1),"regenerable pool peaks stay out of snapshot source metadata");
    db.exec("DELETE FROM audio_clips WHERE clip_id=1");auto removed=SnapshotBuilder::fromStore(*store,*frame);check(!clip(*removed,1)->audio && clip(*frame,1)->audio,"attachment removal changes only new frames");
    db.exec("PRAGMA foreign_keys=OFF;INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(1,999,4);PRAGMA foreign_keys=ON");auto orphan=SnapshotBuilder::fromStore(*store,*removed);
    check(clip(*orphan,1)->audio->mediaId==999 && !clip(*orphan,1)->audio->mediaInfo,"legacy missing pool row retains source address without inventing metadata");
}
}
int main(){try{run();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
