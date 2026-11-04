// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/mixer.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "adi/textproj_store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace adi;
using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
bool near(double a,double b,double tolerance=1e-6){return std::abs(a-b)<tolerance;}
template<class T>struct Audio {
    std::array<std::array<T,512>,4> input{},output{};
    std::array<const T*,4> in{};std::array<T*,4> out{};
    Audio(){for(std::size_t c=0;c<4;++c){input[c].fill(static_cast<T>(0.25+0.5*static_cast<double>(c)));in[c]=input[c].data();out[c]=output[c].data();}}
    NodeIo io(int offset,int frames,bool inplace=false,int channels=2) {
        NodeIo result;result.channels=channels;result.frames=frames;result.blockOffset=offset;result.sampleRate=48000;
        if constexpr(std::is_same_v<T,double>){result.in64=in.data();result.out64=out.data();if(inplace)for(std::size_t c=0;c<4;++c)out[c]=input[c].data();}
        else {result.in=in.data();result.out=out.data();if(inplace)for(std::size_t c=0;c<4;++c)out[c]=input[c].data();}
        return result;
    }
};
template<class T>void matrix() {
    for(const auto& [left,right,expectedLeft,expectedRight]:std::array<std::array<double,4>,5>{{
        {-1,1,0.25,0.75},{1,-1,0.75,0.25},{-1,-1,1,0},{1,1,0,1},{0,0,std::sqrt(0.5),std::sqrt(0.5)}
    }}) {
        StripNode strip;strip.prepare(48000,512);strip.setPanMode(PanMode::Split,left,right);Audio<T> audio;
        strip.process(audio.io(0,64));
        check(near(audio.output[0][0],expectedLeft) && near(audio.output[1][0],expectedRight),"split matrix routes independent source positions in native width");
        StripNode inPlace;inPlace.prepare(48000,512);inPlace.setPanMode(PanMode::Split,left,right);Audio<T> aliased;
        inPlace.process(aliased.io(0,64,true));
        check(near(aliased.input[0][0],expectedLeft) && near(aliased.input[1][0],expectedRight),"in-place crossfeed reads both sources before either output");
    }
    Audio<T> audio;StripNode wider;wider.prepare(48000,512);wider.setPanMode(PanMode::Split,1,-1);wider.process(audio.io(0,64,false,4));
    check(audio.output[2][0]==audio.input[2][0] && audio.output[3][0]==audio.input[3][0],"channels beyond stereo pair retain fader-only gain");
    Audio<T> mono;StripNode one;one.prepare(48000,512);one.setPanMode(PanMode::Split,-1,1);one.process(mono.io(0,64,false,1));
    check(mono.output[0][0]==mono.input[0][0],"missing right channel is silent and mono remains safe");
    Audio<T> muted;StripNode mute;mute.prepare(48000,512);mute.setStatic(0,0,PanLaw::Live,false);mute.setPanMode(PanMode::Split,1,-1);mute.process(muted.io(0,64));
    check(muted.output[0][0]==0 && muted.output[1][0]==0,"mute gates crossfeeds as well as diagonal gains");
    Audio<T> faded;StripNode fader;fader.prepare(48000,512);fader.setStatic(-6.020599913279624,0,PanLaw::Live,true);fader.setPanMode(PanMode::Split,1,-1);fader.process(faded.io(0,64));
    check(near(faded.output[0][0],0.375) && near(faded.output[1][0],0.125),"fader scales both split sources");
    StripConfig config;config.panMode=PanMode::Split;config.panLeft=1;config.panRight=-1;
    SendNode send;send.setSource(config,nullptr);send.set(0,0,PanLaw::Live,true);send.prepare(48000,512);Audio<T> sent;send.process(sent.io(0,64));
    check(near(sent.output[0][0],0.75) && near(sent.output[1][0],0.25),"post send reproduces source strip split matrix");
    config=StripConfig{};SendNode pre;pre.setSource(config,nullptr);pre.set(0,0,PanLaw::Live,true);pre.prepare(48000,512);Audio<T> dry;pre.process(dry.io(0,64));
    check(dry.output[0][0]==dry.input[0][0] && dry.output[1][0]==dry.input[1][0],"pre send's unity source bypasses split panning");
}
void ramps() {
    StripNode whole,pieces;
    for(auto* strip:{&whole,&pieces}){strip->prepare(48000,512);strip->setPanMode(PanMode::Split,-1,1);}
    Audio<double> a,b;whole.process(a.io(0,1));pieces.process(b.io(0,1));
    whole.setPanMode(PanMode::Split,1,-1);pieces.setPanMode(PanMode::Split,1,-1);
    whole.process(a.io(0,256));
    pieces.process(b.io(0,31));pieces.process(b.io(31,65));pieces.process(b.io(96,160));
    bool same=true;for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<256;++i)same=same && a.output[c][i]==b.output[c][i];
    check(same,"crossfeed ramp is invariant to segment partition");
    check(a.output[0][0]>0.25 && a.output[0][0]<0.26 && a.output[1][0]<0.75,"swap begins with a small ramp step");
    check(a.output[0][239]==0.75 && a.output[1][239]==0.25,"all matrix coefficients land exactly at five milliseconds");
    StripNode handoff;handoff.prepare(48000,512);handoff.setPanMode(PanMode::Split,-1,1);Audio<float> narrow;handoff.process(narrow.io(0,1));
    handoff.setPanMode(PanMode::Split,1,-1);handoff.process(narrow.io(0,120));
    Audio<double> wide;handoff.process(wide.io(0,120));
    check(near(wide.output[0][0],0.25+0.5*121/240.0) && wide.output[0][0]>narrow.output[0][119],"precision handoff continues active crossfeed ramp instead of replaying old matrix");
    check(near(wide.output[0][119],0.75) && near(wide.output[1][119],0.25),"handoff lands swap after remaining samples");
    StripNode dormant;dormant.prepare(48000,512);dormant.setStatic(0,1,PanLaw::Live,true);dormant.setPanMode(PanMode::Split,-1,1);Audio<double> identity;dormant.process(identity.io(0,1));
    check(identity.output[0][0]==0.25 && identity.output[1][0]==0.75,"ordinary pan stays dormant in split mode");
}
struct Project {
    test::TempDirectory temporary{"split_pan","ops"};std::unique_ptr<Store> store;
    Project(){StoreError error{};store=Store::create(temporary.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(21,'audio','Split'),(22,'audio','Other')");
        store->db().exec("INSERT INTO mixer_strip(track_id) VALUES(21),(22)");}
    bool op(const char* name,Payload payload){OpRequest request;request.opType=name;request.payload=std::move(payload);return OpJournal(*store).commit(request).ok;}
};
void persistence() {
    Project p;auto& db=p.store->db();auto original=SnapshotBuilder::fromStore(*p.store);
    check(original->tracks[0]->panMode=="stereo" && original->tracks[0]->panLeft==-1 && original->tracks[0]->panRight==1,"absent settings row gives stable stereo/default split positions");
    check(p.op("mixer.setPanMode",{{"id",21},{"mode","split"}}),"mode op commits");
    auto mode=SnapshotBuilder::fromStore(*p.store,*original);
    check(mode->tracks[0]!=original->tracks[0] && mode->tracks[0]->panMode=="split" && mode->tracks[1]==original->tracks[1],"mode participates in sharing while other track stays shared");
    check(History(*p.store).undo().ok && db.execAndGet("SELECT count(*) FROM mixer_pan_settings").getInt()==0,"undo first-touch restores absent extension row");
    check(History(*p.store).redo().ok,"redo mode restores settings");
    check(p.op("mixer.setPanLeft",{{"id",21},{"pan",0.4}}),"left pan op commits");auto left=SnapshotBuilder::fromStore(*p.store,*mode);
    check(left->tracks[0]!=mode->tracks[0] && left->tracks[0]->panLeft==0.4,"left position participates in sharing");
    check(p.op("mixer.setPanRight",{{"id",21},{"pan",-0.6}}),"right pan op commits");auto right=SnapshotBuilder::fromStore(*p.store,*left);
    check(right->tracks[0]!=left->tracks[0] && right->tracks[0]->panRight==-0.6,"right position participates in sharing");
    auto stable=SnapshotBuilder::fromStore(*p.store,*right);check(stable->tracks[0]==right->tracks[0],"unchanged settings share track");
    check(p.op("mixer.setPanMode",{{"id",21},{"mode","stereo"}}) && p.op("mixer.setPanMode",{{"id",21},{"mode","split"}}),"both modes can be revisited");
    const auto projection=textproj::projectStore(*p.store).text;
    check(projection.find("panMode split")!=std::string::npos && projection.find("panLeft 0.4")!=std::string::npos && projection.find("panRight -0.6")!=std::string::npos,"text projection exposes mode and both source positions");
    auto model=rows::readModel(*p.store);check(model.strips[0].panLeft==0.4 && model.strips[0].panRight==-0.6 && model.strips[0].panMode=="split","mode toggles retain both dormant positions in model");
    db.exec("ALTER TABLE mixer_pan_settings ADD COLUMN future_tag BLOB NOT NULL DEFAULT X''");db.exec("UPDATE mixer_pan_settings SET future_tag=X'0001FF'");
    check(p.op("mixer.setPanLeft",{{"id",21},{"pan",-0.2}}) && History(*p.store).undo().ok,"parameter edit and undo update existing settings in place");
    check(db.execAndGet("SELECT hex(future_tag) FROM mixer_pan_settings").getString()=="0001FF" && near(db.execAndGet("SELECT pan_left FROM mixer_pan_settings").getDouble(),0.4),"undo preserves unknown settings columns and prior exact value");
    db.exec("UPDATE mixer_pan_settings SET mode='future'");
    check(p.op("mixer.setPanMode",{{"id",21},{"mode","stereo"}}) && History(*p.store).undo().ok && db.execAndGet("SELECT mode FROM mixer_pan_settings").getString()=="future","inverse restores an unknown future mode verbatim");
    const auto count=db.execAndGet("SELECT count(*) FROM ops").getInt();
    check(!p.op("mixer.setPanMode",{{"id",21},{"mode","bogus"}}) && !p.op("mixer.setPanLeft",{{"id",21},{"pan",1.1}}) &&
          !p.op("mixer.setPanRight",{{"id",999},{"pan",0}}) && !p.op("mixer.setPanRight",{{"id",21},{"pan",std::numeric_limits<double>::infinity()}}),"invalid mode, pan and target are refused");
    check(db.execAndGet("SELECT count(*) FROM ops").getInt()==count,"refused edits do not leave journal rows");
    p.store.reset();StoreError error{};p.store=Store::open(p.temporary.path()/"song.adi",error);
    check(p.store && rows::readModel(*p.store).strips[0].panMode=="future","reopen retains native settings rather than only runtime state");
}
void automationAndCompatibility() {
    AutomationLaneProgram l,r;l.defaultValue=1;r.defaultValue=-1;
    StripLanes lanes;lanes.panLeft=&l;lanes.panRight=&r;
    StripNode strip;Transport transport;strip.prepare(48000,512);strip.setTransport(&transport);
    strip.setPanMode(PanMode::Split,-1,1);strip.setLanes(&lanes,{});Audio<double> audio;strip.process(audio.io(0,16));
    check(audio.output[0][0]==0.75 && audio.output[1][0]==0.25,"split controls follow their own real-unit lanes");
    Project p;auto& db=p.store->db();
    check(p.op("mixer.setPanLeft",{{"id",21},{"pan",0.4}}) && History(*p.store).undo().ok && db.execAndGet("SELECT count(*) FROM mixer_pan_settings").getInt()==0,"first left edit also restores row absence on undo");
    check(p.op("mixer.setPanLeft",{{"id",21},{"pan",0.4}}),"seed stored manual pan before automated session");
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(23,'master','Main')");db.exec("INSERT INTO mixer_strip(track_id) VALUES(23)");
    db.exec("INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) VALUES(1,'track',21,'track',23,'main')");
    db.exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain,default_value) VALUES(90,'track',21,'panLeft','real',1),(91,'track',21,'panRight','real',-1)");
    auto program=compileAutomation(rows::readModel(*p.store),48000);auto bound=bindStripAutomation(program,{});
    check(bound->problems.empty() && bound->byTrack.at(21).panLeft && bound->byTrack.at(21).panRight,"both new references bind to distinct track lanes");
    Session session;check(session.load(*p.store,{},SessionSpec{}),"session accepts split pan lane program");
    check(p.op("mixer.setPanLeft",{{"id",21},{"pan",-0.5}}) && session.refresh(*p.store) && session.overriddenLanes().contains(90) && !session.overriddenLanes().contains(91),"manual left edit overrides only its own lane on refresh");
    check(session.reenableAutomation(90) && !session.automationOverridden(),"Re-Enable resumes split lane normally");
    p.store.reset();
    {SQLite::Database old((p.temporary.path()/"song.adi").string(),SQLite::OPEN_READWRITE);
     {auto gone=adi::tablesAddedAfter(9);for(auto it=gone.rbegin();it!=gone.rend();++it) old.exec("DROP TABLE "+*it);} /* every table since 1.9: the predecessor is 1.9 itself, not this minor alone (chain) */old.exec("UPDATE adi_meta SET value='9' WHERE key='schema_minor'");old.exec("PRAGMA user_version=1009");}
    StoreError error{};p.store=Store::open(p.temporary.path()/"song.adi",error,true);
    check(p.store && p.store->schemaMinor()==9 && p.store->db().execAndGet("SELECT count(*) FROM mixer_pan_settings").getInt()==0,"old read-only project sees empty TEMP settings without migration");
    auto snapshot=SnapshotBuilder::fromStore(*p.store);check(snapshot->tracks[0]->panMode=="stereo" && snapshot->tracks[0]->panLeft==-1 && snapshot->tracks[0]->panRight==1,"old read-only snapshot uses exact stereo defaults");
    check(!p.op("mixer.setPanMode",{{"id",21},{"mode","split"}}),"read-only project refuses editing its compatibility table");
    p.store.reset();{SQLite::Database old((p.temporary.path()/"song.adi").string(),SQLite::OPEN_READONLY);check(old.execAndGet("PRAGMA user_version").getInt()==1009 && old.execAndGet("SELECT count(*) FROM sqlite_master WHERE name='mixer_pan_settings'").getInt()==0,"read-only open leaves persistent version and schema untouched");}
    p.store=Store::open(p.temporary.path()/"song.adi",error);
    check(p.store && p.store->schemaMinor()==adi::kSchemaMinor && p.store->upgradedFromMinor()==9 && p.store->db().execAndGet("SELECT count(*) FROM mixer_pan_settings").getInt()==0,"writable old project migrates additively to native pan settings");
}

} // namespace
int main(){try{matrix<float>();matrix<double>();ramps();persistence();automationAndCompatibility();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
