// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
namespace adi {
struct CueSettings {
    bool cueMode=false,soloInPlace=true,referenceOn=false,referenceFromRole=true;
    int firstOutputChannel=0;
    double volumeDb=0;
    std::int64_t referenceTrack=0;
    std::int64_t projectReferenceTrack=0; // derived from tracks.role when that format field exists
    std::vector<std::int64_t> selectedTracks;
    friend bool operator==(const CueSettings&,const CueSettings&)=default;
};
CueSettings readCueSettings(const Store&);
bool cueModeApply(OpContext&,const Payload&,std::string&);
bool cueOutputApply(OpContext&,const Payload&,std::string&);
bool cueVolumeApply(OpContext&,const Payload&,std::string&);
bool cueTrackApply(OpContext&,const Payload&,std::string&);
bool cueReferenceApply(OpContext&,const Payload&,std::string&);
bool soloInPlaceApply(OpContext&,const Payload&,std::string&);
}
