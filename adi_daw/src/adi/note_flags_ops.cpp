// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/note_flags_ops.hpp"
#include "adi/blob.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace adi::note_flags {
namespace {
std::int64_t clipId(const Payload& payload) {
    const auto& value=payload.at("clip");
    if(!value.is_number_integer() || (value.is_number_unsigned() &&
       value.get<std::uint64_t>()>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        throw std::runtime_error("clip id outside signed 64-bit range");
    return value.get<std::int64_t>();
}
std::uint64_t noteId(const Payload& payload) {
    const auto& value=payload.at("note");
    std::uint64_t id=0;
    if(value.is_number_unsigned())id=value.get<std::uint64_t>();
    else if(value.is_number_integer() && value.get<std::int64_t>()>0)id=static_cast<std::uint64_t>(value.get<std::int64_t>());
    if(id==0)throw std::runtime_error("note id must be assigned and nonnegative");
    return id;
}
struct Target {
    std::int64_t streamId=0;
    std::vector<std::byte> bytes;
    std::size_t flagsOffset=0;
    std::uint16_t flags=0;
};
Target read(OpContext& context,const Payload& payload) {
    const auto clip=clipId(payload);const auto wanted=noteId(payload);
    SQLite::Statement owner(context.db,"SELECT kind FROM clips WHERE id=?");owner.bind(1,clip);
    if(!owner.executeStep() || owner.getColumn(0).getString()!="midi")throw std::runtime_error("note mute needs a MIDI clip");
    SQLite::Statement query(context.db,"SELECT id,data FROM event_streams WHERE clip_id=? AND stream_kind='notes' "
                                       "AND channel IS NULL AND controller IS NULL");query.bind(1,clip);
    if(!query.executeStep())throw std::runtime_error("clip has no note stream");
    Target result;result.streamId=query.getColumn(0).getInt64();
    const auto blob=query.getColumn(1);
    if(blob.getBytes()>0) {
        const auto* begin=static_cast<const std::byte*>(blob.getBlob());
        result.bytes.assign(begin,begin+blob.getBytes());
    }
    const StreamReader<NoteRecord> reader(result.bytes,FourCC::Notes);
    if(!reader.ok())throw std::runtime_error(std::string("unreadable note stream: ")+toString(reader.error()));
    if((reader.header().flags & ~StreamFlags::SortedByTime)!=0)throw std::runtime_error("unsupported note stream flags");
    bool found=false;
    for(std::uint32_t index=0;index<reader.count();++index) {
        const auto note=reader.at(index);
        if(!note)throw std::runtime_error("unreadable note record");
        if(note->note_id!=wanted)continue;
        if(found)throw std::runtime_error("ambiguous duplicate note id");
        found=true;result.flags=note->flags;
        result.flagsOffset=sizeof(StreamHeader)+static_cast<std::size_t>(index)*reader.header().rec_size+offsetof(NoteRecord,flags);
    }
    if(!found)throw std::runtime_error("no such note");
    return result;
}
} // namespace

bool mute(OpContext& context,const Payload& payload,std::string& error) {
    try {
        auto target=read(context,payload);
        constexpr auto keep=static_cast<std::uint16_t>(~NoteFlags::Muted);
        const auto flags=payload.at("muted").get<bool>()
            ? static_cast<std::uint16_t>(target.flags|NoteFlags::Muted)
            : static_cast<std::uint16_t>(target.flags&keep);
        // Patch only this field in its ORIGINAL stride. Sorting/re-encoding a
        // NoteRecord vector would discard future tails and unknown header data.
        std::memcpy(target.bytes.data()+target.flagsOffset,&flags,sizeof flags);
        if(target.bytes.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::runtime_error("note stream too large");
        SQLite::Statement update(context.db,"UPDATE event_streams SET data=? WHERE id=?");
        update.bindNoCopy(1,target.bytes.data(),static_cast<int>(target.bytes.size()));update.bind(2,target.streamId);
        if(update.exec()!=1)throw std::runtime_error("note stream disappeared");
        return true;
    } catch(const std::exception& e){error=e.what();return false;}
}
bool muteInverse(OpContext& context,const Payload& payload,Payload& inverse,std::string& error) {
    try {
        const auto target=read(context,payload);
        inverse={{"clip",clipId(payload)},{"note",noteId(payload)},{"muted",(target.flags&NoteFlags::Muted)!=0}};
        return true;
    } catch(const std::exception& e){error=e.what();return false;}
}
} // namespace adi::note_flags
