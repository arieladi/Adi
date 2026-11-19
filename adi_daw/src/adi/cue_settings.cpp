// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/cue_settings.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cmath>
#include <algorithm>
namespace adi {
namespace {
bool valid(const CueSettings& s) {
    return s.firstOutputChannel>=0 && s.firstOutputChannel<=62 && s.firstOutputChannel%2==0 && std::isfinite(s.volumeDb) && s.volumeDb>=-150 && s.volumeDb<=6 &&
        s.referenceTrack>=0 && (!s.referenceOn || s.referenceTrack!=0) &&
        std::is_sorted(s.selectedTracks.begin(),s.selectedTracks.end()) &&
        std::adjacent_find(s.selectedTracks.begin(),s.selectedTracks.end())==s.selectedTracks.end() &&
        std::all_of(s.selectedTracks.begin(),s.selectedTracks.end(),[](std::int64_t id){return id>0;});
}
Payload value(const CueSettings& s) {
    return {{"cue",s.cueMode},{"sip",s.soloInPlace},{"first",s.firstOutputChannel},{"db",s.volumeDb},{"reference",s.referenceFromRole?0:s.referenceTrack},{"reference_on",s.referenceOn},{"selected",s.selectedTracks}};
}
bool update(OpContext& c,const Payload& p,std::string& error,int field) {
    try {
        auto s=readCueSettings(c.store);
        switch(field) {
            case 0:s.cueMode=p.at("on").get<bool>();break;
            case 1:{
                const auto first=p.at("first").get<std::int64_t>();
                if(first<0 || first>62){error="Cue Out is a stereo pair from 0 through 62";return false;}
                s.firstOutputChannel=static_cast<int>(first);break;
            }
            case 2:s.volumeDb=p.at("db").get<double>();break;
            case 3:
                s.referenceOn=p.at("on").get<bool>();
                if(p.contains("id")){const auto id=p.at("id").get<std::int64_t>();s.referenceFromRole=id==0;s.referenceTrack=s.referenceFromRole?s.projectReferenceTrack:id;}
                break;
            case 4:s.soloInPlace=p.at("on").get<bool>();break;
            case 5:{
                const auto id=p.at("id").get<std::int64_t>();
                SQLite::Statement track(c.db,"SELECT kind FROM tracks WHERE id=?");track.bind(1,id);
                if(id<=0 || !track.executeStep()){error="Cue target does not exist";return false;}
                const auto kind=track.getColumn(0).getString();
                if(kind!="audio" && kind!="midi" && kind!="instrument" && kind!="group" && kind!="return"){error="Cue target carries no monitor audio";return false;}
                const auto at=std::lower_bound(s.selectedTracks.begin(),s.selectedTracks.end(),id);
                if(p.at("on").get<bool>()){if(at==s.selectedTracks.end() || *at!=id)s.selectedTracks.insert(at,id);}
                else if(at!=s.selectedTracks.end() && *at==id)s.selectedTracks.erase(at);
                break;
            }
            default:error="unknown cue field";return false;
        }
        if(!valid(s)){error="invalid cue output, gain or reference selection";return false;}
        if(field==3 && s.referenceTrack) {
            SQLite::Statement track(c.db,"SELECT kind FROM tracks WHERE id=?");track.bind(1,s.referenceTrack);
            if(!track.executeStep()){error="reference track does not exist";return false;}
            const auto kind=track.getColumn(0).getString();
            if(kind!="audio" && kind!="midi" && kind!="instrument" && kind!="group" && kind!="return"){error="reference must be an audio-carrying non-Main track";return false;}
        }
        SQLite::Statement q(c.db,"INSERT INTO session_state(key,value) VALUES('cue.controls',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        q.bind(1,value(s).dump());q.exec();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
}
CueSettings readCueSettings(const Store& store) {
    CueSettings s;
    // Read an optional format field without requiring its migration to land
    // with this monitor backend. Win owns the tracks.role placement (N38).
    bool hasRole=false;
    auto& db=const_cast<SQLite::Database&>(store.db());
    {SQLite::Statement columns(db,"PRAGMA table_info(tracks)");while(columns.executeStep())hasRole=hasRole || columns.getColumn(1).getString()=="role";}
    if(hasRole){SQLite::Statement role(db,"SELECT id FROM tracks WHERE role='reference' AND id>0 AND kind IN ('audio','midi','instrument','group','return') ORDER BY id LIMIT 1");if(role.executeStep())s.projectReferenceTrack=role.getColumn(0).getInt64();}
    s.referenceTrack=s.projectReferenceTrack;
    const auto defaults=s;
    SQLite::Statement q(const_cast<SQLite::Database&>(store.db()),"SELECT value FROM session_state WHERE key='cue.controls'");
    if(!q.executeStep())return s;
    try {
        const auto p=Payload::parse(q.getColumn(0).getString());
        s.cueMode=p.at("cue").get<bool>();s.soloInPlace=p.at("sip").get<bool>();s.firstOutputChannel=p.at("first").get<int>();
        s.volumeDb=p.at("db").get<double>();s.referenceTrack=p.at("reference").get<std::int64_t>();s.referenceOn=p.at("reference_on").get<bool>();
        s.referenceFromRole=s.referenceTrack==0;
        if(s.referenceFromRole)s.referenceTrack=s.projectReferenceTrack;
        if(p.contains("selected"))s.selectedTracks=p.at("selected").get<std::vector<std::int64_t>>();
        return valid(s)?s:defaults;
    }catch(const std::exception&){return defaults;}
}
bool cueModeApply(OpContext& c,const Payload& p,std::string& e){return update(c,p,e,0);}
bool cueOutputApply(OpContext& c,const Payload& p,std::string& e){return update(c,p,e,1);}
bool cueVolumeApply(OpContext& c,const Payload& p,std::string& e){return update(c,p,e,2);}
bool cueTrackApply(OpContext& c,const Payload& p,std::string& e){return update(c,p,e,5);}
bool cueReferenceApply(OpContext& c,const Payload& p,std::string& e){return update(c,p,e,3);}
bool soloInPlaceApply(OpContext& c,const Payload& p,std::string& e){return update(c,p,e,4);}
}
