// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/note_diff.hpp"
#include "adi/note_diff_ops.hpp"
#include "adi/note_stream_detail.hpp"
#include "adi/edit_rows.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace adi::notes {
namespace {
struct Read {
    std::optional<SQLite::Transaction> transaction;
    explicit Read(SQLite::Database& db) {
        if(sqlite3_get_autocommit(db.getHandle())!=0)transaction.emplace(db,SQLite::TransactionBehavior::DEFERRED);
    }
};
std::string filename(Store& store) {
    SQLite::Statement query(store.db(),"PRAGMA database_list");
    while(query.executeStep())if(query.getColumn(1).getString()=="main")return query.getColumn(2).getString();
    throw std::runtime_error("note editing needs a project connection");
}
std::int64_t integer(const Payload& value) {
    std::string error;const auto result=detail::signedValue(value,error);
    if(!result)throw std::runtime_error(error);
    return *result;
}
std::uint64_t noteId(const Payload& value) {
    std::string error;const auto result=detail::noteIdValue(value,error);
    if(!result)throw std::runtime_error(error);
    return *result;
}
bool sameSqlCell(const Payload& left,const Payload& right) {
    if(left.is_null() || right.is_null())return left.is_null() && right.is_null();
    // CBOR can decode a positive SQL INTEGER as unsigned. Both JSON integer
    // categories represent the same SQL storage class; REAL is distinct.
    if(left.is_number_integer() || right.is_number_integer())
        return left.is_number_integer() && right.is_number_integer() && integer(left)==integer(right);
    if(left.is_number_float() || right.is_number_float())
        return left.is_number_float() && right.is_number_float() && left.get<double>()==right.get<double>();
    if(left.is_string() || right.is_string())return left.is_string() && right.is_string() && left==right;
    if(left.is_binary() || right.is_binary())return left.is_binary() && right.is_binary() && left==right;
    return false;
}
bool sameSqlRow(const Payload& left,const Payload& right) {
    if(!left.is_object() || !right.is_object() || left.size()!=right.size())return false;
    for(auto item=left.begin();item!=left.end();++item)
        if(!right.contains(item.key()) || !sameSqlCell(item.value(),right.at(item.key())))return false;
    return true;
}
bool sameSqlRow(const std::optional<Payload>& left,const std::optional<Payload>& right) {
    if(!left || !right)return left.has_value()==right.has_value();
    return sameSqlRow(*left,*right);
}
bool sameSqlRows(const std::vector<Payload>& left,const std::vector<Payload>& right) {
    if(left.size()!=right.size())return false;
    for(std::size_t index=0;index<left.size();++index)if(!sameSqlRow(left[index],right[index]))return false;
    return true;
}
void closed(const Payload& value,std::initializer_list<std::string_view> required,
            std::initializer_list<std::string_view> optional={}) {
    if(!value.is_object())throw std::runtime_error("note packet must be an object");
    for(const auto field:required)if(!value.contains(std::string(field)))throw std::runtime_error("missing note packet field: "+std::string(field));
    for(auto item=value.begin();item!=value.end();++item) {
        if(std::find(required.begin(),required.end(),item.key())==required.end() &&
           std::find(optional.begin(),optional.end(),item.key())==optional.end())
            throw std::runtime_error("unknown note packet field: "+item.key());
    }
}
void noSqlNan(const Payload& row) {
    if(!row.is_object())throw std::runtime_error("captured SQL row must be an object");
    for(const auto& cell:row)if(cell.is_number_float() && std::isnan(cell.get<double>()))
        throw std::runtime_error("captured SQL REAL NaN would change storage type to NULL");
}
Payload blob(std::span<const std::byte> bytes) {
    std::vector<std::uint8_t> result;result.reserve(bytes.size());
    for(const auto byte:bytes)result.push_back(std::to_integer<std::uint8_t>(byte));
    return Payload::binary(std::move(result));
}
std::vector<std::byte> blobBytes(const Payload& value) {
    if(!value.is_binary())throw std::runtime_error("captured ANOT data must be a typed SQLite BLOB");
    const auto& raw=value.get_binary();
    if(raw.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::runtime_error("ANOT exceeds SQLite BLOB size");
    std::vector<std::byte> result;result.reserve(raw.size());
    for(const auto byte:raw)result.push_back(static_cast<std::byte>(byte));
    return result;
}
std::vector<std::byte> nativeEmpty() {
    std::string error;auto result=detail::encodeNotes(detail::emptyNotes(),error);
    if(!result)throw std::runtime_error(error);
    return std::move(*result);
}
NoteValues recordValues(const Record& record) {
    std::string error;auto result=values(record,error);
    if(!result)throw std::runtime_error(error);
    return *result;
}
Record parseRecord(const Payload& packet) {
    std::string error;auto result=detail::parseRecordPacket(packet,error);
    if(!result)throw std::runtime_error(error);
    return std::move(*result);
}
Payload clipRow(SQLite::Database& db,std::int64_t clip) {
    const auto row=adi::detail::captureRow(db,"clips","id",clip);
    if(!row || row->at("kind")!="midi")throw std::runtime_error("note editing needs an existing MIDI clip");
    return *row;
}
std::optional<Payload> canonicalRow(SQLite::Database& db,std::int64_t clip) {
    SQLite::Statement query(db,"SELECT * FROM event_streams WHERE clip_id=? AND stream_kind='notes' AND channel IS NULL AND controller IS NULL ORDER BY id");
    query.bind(1,clip);if(!query.executeStep())return std::nullopt;
    auto result=adi::detail::captureQueryRow(query);
    if(query.executeStep())throw std::runtime_error("MIDI clip has multiple canonical notes streams");
    return result;
}
std::vector<Payload> expressions(SQLite::Database& db,std::int64_t clip) {
    std::vector<Payload> result;SQLite::Statement query(db,"SELECT * FROM note_expression WHERE clip_id=? ORDER BY id");
    query.bind(1,clip);while(query.executeStep())result.push_back(adi::detail::captureQueryRow(query));
    return result;
}
detail::CapturedNotes capture(SQLite::Database& db,std::int64_t clip) {
    detail::CapturedNotes result;result.clipRow=clipRow(db,clip);result.streamRow=canonicalRow(db,clip);
    result.expressionRows=expressions(db,clip);return result;
}
detail::SnapshotData snapshot(detail::CapturedNotes captured) {
    auto value=detail::makeSnapshot(std::move(captured));
    if(!value.ok)throw std::runtime_error(value.error);
    const auto* data=detail::SnapshotAccess::get(value);
    if(!data)throw std::runtime_error("note capture is unavailable");
    return *data;
}
void canonical(const Payload& row,std::int64_t clip) {
    noSqlNan(row);
    if(!row.is_object() || integer(row.at("clip_id"))!=clip || row.at("stream_kind")!="notes" ||
       !row.at("channel").is_null() || !row.at("controller").is_null())
        throw std::runtime_error("captured stream is not this clip's canonical notes row");
    (void)integer(row.at("id"));
}
std::set<std::uint64_t> identities(const std::optional<Payload>& row,std::int64_t clip) {
    std::set<std::uint64_t> result;if(!row)return result;
    canonical(*row,clip);std::string error;auto stream=detail::decodeNotes(blobBytes(row->at("data")),error);
    if(!stream)throw std::runtime_error(error);
    for(const auto& record:stream->records)result.insert(recordValues(record).id);
    return result;
}
std::vector<Payload> selectedExpressions(const std::vector<Payload>& rows,std::uint64_t id) {
    std::vector<Payload> result;const auto sqlId=detail::expressionSqlId(id);
    for(const auto& row:rows)if(integer(row.at("note_id"))==sqlId)result.push_back(row);
    return result;
}
using ExpressionIndex=std::map<std::uint64_t,std::vector<Payload>>;
ExpressionIndex indexExpressions(const std::vector<Payload>& rows) {
    ExpressionIndex result;
    for(const auto& row:rows)result[detail::expressionNoteId(integer(row.at("note_id")))].push_back(row);
    return result;
}
const std::vector<Payload>& expressionRows(const ExpressionIndex& index,std::uint64_t id) {
    static const std::vector<Payload> empty;
    const auto found=index.find(id);return found==index.end()?empty:found->second;
}
std::vector<Payload> orderedRows(const Payload& rows) {
    if(!rows.is_array())throw std::runtime_error("expression set must be an array");
    std::map<std::int64_t,Payload> ordered;
    for(const auto& row:rows) {
        if(!row.is_object() || !ordered.emplace(integer(row.at("id")),row).second)
            throw std::runtime_error("expression set contains duplicate or invalid row identities");
    }
    std::vector<Payload> result;result.reserve(ordered.size());
    for(auto& [id,row]:ordered){(void)id;result.push_back(std::move(row));}
    return result;
}
struct RowPair {std::optional<Payload> before,after;};
struct Plan {
    std::int64_t clip=0;
    RowPair stream;
    std::vector<RowPair> expressions;
};
Payload rowChanges(const Plan& plan) {
    Payload changes=Payload::array();
    auto add=[&](const char* table,const RowPair& pair) {
        if(pair.before==pair.after)return;
        const auto& row=pair.before?*pair.before:*pair.after;
        changes.push_back({{"table",table},{"id",integer(row.at("id"))},
            {"before",pair.before.value_or(Payload(nullptr))},{"after",pair.after.value_or(Payload(nullptr))}});
    };
    add("event_streams",plan.stream);for(const auto& pair:plan.expressions)add("note_expression",pair);
    return changes;
}
void validatePlan(OpContext& context,const Plan& plan) {
    const auto changes=rowChanges(plan);if(changes.empty())return;
    std::string error;if(!edit::validateRows(context,{{"changes",changes}},error))throw std::runtime_error(error);
}
void applyPlan(OpContext& context,const Plan& plan) {
    const auto changes=rowChanges(plan);if(changes.empty())return;
    // Keep the complete owner set, including unrelated orphan identities.
    // Checking only rows named in the diff would miss trigger-created rows.
    std::map<std::int64_t,Payload> expectedExpressions;
    for(auto& row:expressions(context.db,plan.clip))expectedExpressions.emplace(integer(row.at("id")),std::move(row));
    for(const auto& pair:plan.expressions) {
        if(pair.before)expectedExpressions.erase(integer(pair.before->at("id")));
        if(pair.after)expectedExpressions.emplace(integer(pair.after->at("id")),*pair.after);
    }
    if(changes.size()>static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("note row change count exceeds SQLite accounting range");
    const auto writtenBefore=sqlite3_total_changes64(context.db.getHandle());
    if(writtenBefore<0)throw std::runtime_error("SQLite change counter is outside its supported range");
    std::string error;if(!edit::applyRows(context,{{"changes",changes}},error))throw std::runtime_error(error);
    const auto writtenAfter=sqlite3_total_changes64(context.db.getHandle());
    if(writtenAfter<writtenBefore || writtenAfter-writtenBefore!=static_cast<std::int64_t>(changes.size()))
        throw std::runtime_error("SQL performed extra or missing note row writes");
    // SQL affinity and triggers must not turn literal captured values into a
    // different after-image. Check the actual storage classes before the
    // caller's journal/history transaction can commit; never repair by writing
    // the stream a second time.
    for(const auto& change:changes) {
        const auto actual=adi::detail::captureRow(context.db,change.at("table").get<std::string>(),"id",integer(change.at("id")));
        const auto expected=change.at("after").is_null()?std::optional<Payload>{}:std::optional<Payload>{change.at("after")};
        if(!sameSqlRow(actual,expected))throw std::runtime_error("SQL storage coercion or trigger changed the captured note after-image");
    }
    if(!sameSqlRow(canonicalRow(context.db,plan.clip),plan.stream.after))
        throw std::runtime_error("trigger changed the canonical notes stream after application");
    std::vector<Payload> expected;expected.reserve(expectedExpressions.size());
    for(auto& [id,row]:expectedExpressions){(void)id;expected.push_back(std::move(row));}
    if(!sameSqlRows(expressions(context.db,plan.clip),expected))
        throw std::runtime_error("trigger changed the complete clip expression set after application");
}
Plan fromDiff(OpContext& context,const detail::SnapshotData& captured,const Diff& diff,bool historicalScalar=false) {
    const auto planned=detail::planRecords(captured.baseline,diff);
    if(!planned.ok)throw std::runtime_error(planned.error);
    Plan result;result.clip=captured.clip;result.stream.before=captured.captured.streamRow;
    result.stream.after=result.stream.before;
    const auto expressionIndex=indexExpressions(captured.captured.expressionRows);
    for(const auto& added:planned.effects.added) {
        if(!historicalScalar && !expressionRows(expressionIndex,recordValues(added).id).empty())
            throw std::runtime_error("new note identity already owns orphan expression rows");
    }
    if(!planned.changed)return result;
    if(!result.stream.after) {
        if(!captured.captured.creationRow)throw std::runtime_error("new notes stream needs an explicitly captured creation row");
        result.stream.after=captured.captured.creationRow;
    }
    (*result.stream.after)["data"]=blob(planned.bytes);
    if(!historicalScalar) {
        for(const auto& removed:planned.effects.removed)
            for(const auto& row:expressionRows(expressionIndex,recordValues(removed).id))result.expressions.push_back({row,std::nullopt});
    }
    validatePlan(context,result);return result;
}
Plan forwardPlan(OpContext& context,const Payload& packet) {
    closed(packet,{"clip","stream_id","from","added","removed","changed"},{"create"});
    const auto clip=integer(packet.at("clip")),streamId=integer(packet.at("stream_id"));
    if(!packet.at("from").is_string())throw std::runtime_error("note diff from must be a text hash or absent");
    auto captured=capture(context.db,clip);
    if(captured.streamRow) {
        if(packet.contains("create"))throw std::runtime_error("existing notes stream cannot carry create");
        if(integer(captured.streamRow->at("id"))!=streamId)throw std::runtime_error("canonical notes stream identity changed");
    } else {
        if(packet.at("from")!="absent" || !packet.contains("create") || !packet.at("create").is_object())
            throw std::runtime_error("absent notes stream needs captured create and from=absent");
        const auto& create=packet.at("create");canonical(create,clip);
        if(integer(create.at("id"))!=streamId)throw std::runtime_error("captured creation stream identity differs");
        if(adi::detail::captureRow(context.db,"event_streams","id",streamId))throw std::runtime_error("captured creation stream ID is occupied");
        if(blobBytes(create.at("data"))!=nativeEmpty())throw std::runtime_error("captured creation baseline is not native empty ANOT");
        captured.creationRow=create;
        // Validate complete SQL column shape even for a submitted empty diff;
        // this is validation only, never a second default evaluation or write.
        Plan creation;creation.clip=clip;creation.stream.after=create;validatePlan(context,creation);
    }
    auto value=snapshot(std::move(captured));
    if(packet.at("from")!=value.from)throw std::runtime_error("notes stream bytes changed after capture");
    Diff diff;
    const auto expressionIndex=indexExpressions(value.captured.expressionRows);
    for(const char* field:{"added","removed","changed"})if(!packet.at(field).is_array())throw std::runtime_error("note diff sections must be arrays");
    for(const auto& added:packet.at("added"))diff.added.push_back(parseRecord(added));
    for(const auto& removed:packet.at("removed")) {
        closed(removed,{"record","expressions"});auto record=parseRecord(removed.at("record"));
        if(!sameSqlRows(orderedRows(removed.at("expressions")),expressionRows(expressionIndex,recordValues(record).id)))
            throw std::runtime_error("removed note expression set changed after capture");
        diff.removed.push_back(std::move(record));
    }
    for(const auto& changed:packet.at("changed")) {
        closed(changed,{"old","new"});diff.changed.push_back({parseRecord(changed.at("old")),parseRecord(changed.at("new"))});
    }
    return fromDiff(context,value,diff);
}
Payload inverseCapture(const Plan& plan) {
    Payload stream=nullptr,expression=Payload::array();
    if(plan.stream.before!=plan.stream.after)stream={{"before",plan.stream.after.value_or(Payload(nullptr))},{"after",plan.stream.before.value_or(Payload(nullptr))}};
    for(const auto& pair:plan.expressions)if(pair.before!=pair.after)
        expression.push_back({{"before",pair.after.value_or(Payload(nullptr))},{"after",pair.before.value_or(Payload(nullptr))}});
    return {{"clip",plan.clip},{"note_capture",{{"version",1},{"stream",stream},{"expressions",expression}}}};
}
std::optional<Payload> optionalRow(const Payload& value) {
    if(value.is_null())return std::nullopt;
    if(!value.is_object())throw std::runtime_error("captured row must be an object or null");
    noSqlNan(value);
    return value;
}
RowPair parsePair(const Payload& packet) {
    closed(packet,{"before","after"});RowPair result{optionalRow(packet.at("before")),optionalRow(packet.at("after"))};
    if(!result.before && !result.after)throw std::runtime_error("captured row pair has no row");
    if(result.before && result.after && integer(result.before->at("id"))!=integer(result.after->at("id")))
        throw std::runtime_error("captured row identity changes");
    return result;
}
Plan restorePlan(OpContext& context,const Payload& packet) {
    closed(packet,{"clip","note_capture"});Plan result;result.clip=integer(packet.at("clip"));
    (void)clipRow(context.db,result.clip);
    const auto& captureValue=packet.at("note_capture");closed(captureValue,{"version","stream","expressions"});
    if(integer(captureValue.at("version"))!=1)throw std::runtime_error("unsupported note capture version");
    if(!captureValue.at("expressions").is_array())throw std::runtime_error("captured expression changes must be an array");
    if(captureValue.at("stream").is_null()) {
        if(!captureValue.at("expressions").empty())throw std::runtime_error("expression changes require a captured stream identity transition");
        return result;
    }
    result.stream=parsePair(captureValue.at("stream"));
    if(!sameSqlRow(result.stream.before,canonicalRow(context.db,result.clip)))throw std::runtime_error("captured current notes row changed");
    const auto beforeIds=identities(result.stream.before,result.clip),afterIds=identities(result.stream.after,result.clip);
    if(result.stream.before && result.stream.after) {
        auto before=*result.stream.before,after=*result.stream.after;before.erase("data");after.erase("data");
        if(!sameSqlRow(before,after))throw std::runtime_error("note capture changes stream metadata");
    }
    std::set<std::uint64_t> removed,added,affected;
    std::set_difference(beforeIds.begin(),beforeIds.end(),afterIds.begin(),afterIds.end(),std::inserter(removed,removed.end()));
    std::set_difference(afterIds.begin(),afterIds.end(),beforeIds.begin(),beforeIds.end(),std::inserter(added,added.end()));
    affected=removed;affected.insert(added.begin(),added.end());
    std::map<std::int64_t,RowPair> ordered;
    std::map<std::uint64_t,std::vector<Payload>> expected;
    for(const auto& item:captureValue.at("expressions")) {
        auto pair=parsePair(item);
        if(pair.before && pair.after)throw std::runtime_error("note capture cannot rewrite an existing expression curve");
        const auto& row=pair.before?*pair.before:*pair.after;
        if(integer(row.at("clip_id"))!=result.clip)throw std::runtime_error("captured expression belongs to another clip");
        const auto id=detail::expressionNoteId(integer(row.at("note_id")));
        if(pair.before?!removed.contains(id):!added.contains(id))throw std::runtime_error("captured expression identity is not added or removed by its stream");
        if(!ordered.emplace(integer(row.at("id")),pair).second)throw std::runtime_error("duplicate captured expression row identity");
    }
    for(auto& [id,pair]:ordered) {
        (void)id;
        if(pair.before)expected[detail::expressionNoteId(integer(pair.before->at("note_id")))].push_back(*pair.before);
        result.expressions.push_back(std::move(pair));
    }
    const auto currentExpressions=indexExpressions(expressions(context.db,result.clip));
    for(const auto id:affected)if(!sameSqlRows(expressionRows(currentExpressions,id),expected[id]))
        throw std::runtime_error("captured expression set changed, including an expected empty set");
    validatePlan(context,result);return result;
}
enum class Legacy { Insert, Delete, Move, Resize, Velocity };
bool samePlan(const Plan& left,const Plan& right) {
    if(left.clip!=right.clip)return false;
    using Key=std::pair<std::string,std::int64_t>;
    auto indexed=[](const Plan& plan) {
        std::map<Key,RowPair> result;
        for(const auto& change:rowChanges(plan))result.emplace(Key{change.at("table").get<std::string>(),integer(change.at("id"))},
            RowPair{optionalRow(change.at("before")),optionalRow(change.at("after"))});
        return result;
    };
    const auto a=indexed(left),b=indexed(right);if(a.size()!=b.size())return false;
    for(const auto& [key,pair]:a) {
        const auto found=b.find(key);
        if(found==b.end() || !sameSqlRow(pair.before,found->second.before) || !sameSqlRow(pair.after,found->second.after))return false;
    }
    return true;
}
std::optional<Plan> capturedForwardPlan(OpContext& context,const Payload& packet,std::string_view expected,bool requireCapture) {
    if(!context.replay || context.replay->asInverse)return std::nullopt;
    const auto& replay=*context.replay;
    if(replay.opType!=expected)throw std::runtime_error("note replay opcode and handler disagree");
    if(!replay.hasInverse)throw std::runtime_error("note replay requires its recorded inverse");
    std::string error;auto inverse=decodeFromLog(replay.inverse,error);
    if(!inverse)throw std::runtime_error("cannot decode recorded note inverse: "+error);
    if(!inverse->contains("note_capture")) {
        if(requireCapture)throw std::runtime_error("note diff replay requires its recorded capture inverse");
        return std::nullopt;
    }
    closed(*inverse,{"clip","note_capture"});
    if(integer(inverse->at("clip"))!=integer(packet.at("clip")))throw std::runtime_error("recorded note capture belongs to a different clip");
    auto& captureValue=(*inverse)["note_capture"];closed(captureValue,{"version","stream","expressions"});
    if(!captureValue.at("stream").is_null()) {
        (void)parsePair(captureValue.at("stream"));
        std::swap(captureValue["stream"]["before"],captureValue["stream"]["after"]);
    }
    if(!captureValue.at("expressions").is_array())throw std::runtime_error("captured expression changes must be an array");
    for(auto& pair:captureValue["expressions"]) {
        (void)parsePair(pair);std::swap(pair["before"],pair["after"]);
    }
    return restorePlan(context,*inverse);
}
std::optional<Plan> capturedReplayPlan(OpContext& context,const Payload& packet,Legacy action) {
    if(action==Legacy::Velocity)return std::nullopt;
    const std::string_view expected=action==Legacy::Insert?"note.insert":action==Legacy::Delete?"note.delete":
        action==Legacy::Move?"note.move":"note.resize";
    return capturedForwardPlan(context,packet,expected,false);
}
Plan checkedForwardPlan(OpContext& context,const Payload& packet) {
    const auto expected=capturedForwardPlan(context,packet,"note.applyDiff",true);
    auto planned=forwardPlan(context,packet);
    if(expected && !samePlan(planned,*expected))throw std::runtime_error("stored note diff contradicts its captured inverse");
    return planned;
}
Payload nativeCreation(SQLite::Database& db,std::int64_t clip,bool historicalScalar) {
    const std::set<std::string> native{"id","clip_id","stream_kind","channel","controller","data"};
    std::set<std::string> actual;SQLite::Statement columns(db,"SELECT name,hidden FROM pragma_table_xinfo('event_streams')");
    while(columns.executeStep()) {
        if(columns.getColumn(1).getInt()!=0)throw std::runtime_error("legacy first note insert needs native columns; use prepared note.applyDiff creation");
        actual.insert(columns.getColumn(0).getString());
    }
    if(actual!=native)throw std::runtime_error("legacy first note insert needs native columns; use prepared note.applyDiff creation");
    SQLite::Statement ids(db,"SELECT MAX(id) FROM event_streams");ids.executeStep();
    const auto stored=ids.getColumn(0).isNull()?std::int64_t{0}:ids.getColumn(0).getInt64();
    // Old omitted INTEGER PRIMARY KEY allocation used MAX+1, including zero
    // or negative IDs when only negative keys existed. New capture is literal.
    const auto greatest=historicalScalar?stored:std::max<std::int64_t>(0,stored);
    if(greatest==std::numeric_limits<std::int64_t>::max())throw std::runtime_error("notes stream IDs are exhausted");
    return {{"id",greatest+1},{"clip_id",clip},{"stream_kind","notes"},{"channel",nullptr},{"controller",nullptr},{"data",blob(nativeEmpty())}};
}
int smallInteger(const Payload& value) {
    const auto parsed=integer(value);
    if(parsed<std::numeric_limits<int>::min() || parsed>std::numeric_limits<int>::max())throw std::runtime_error("note field exceeds checked integer range");
    return static_cast<int>(parsed);
}
bool historicalScalarReplay(OpContext& context,const Payload& packet,Legacy action) {
    if(!context.replay)return false;
    const auto& replay=*context.replay;
    if(replay.opType!="note.insert" && replay.opType!="note.delete")return false;
    if(!replay.hasInverse)throw std::runtime_error("legacy note replay requires its recorded inverse");
    const bool inserted=replay.opType=="note.insert";
    const auto expected=(inserted!=replay.asInverse)?Legacy::Insert:Legacy::Delete;
    if(action!=expected)throw std::runtime_error("note replay operation and handler direction disagree");
    std::string error;const auto inverse=decodeFromLog(replay.inverse,error);
    if(!inverse)throw std::runtime_error("cannot decode recorded note inverse: "+error);
    if(inverse->contains("note_capture")) {
        closed(*inverse,{"clip","note_capture"});const auto& captureValue=inverse->at("note_capture");
        closed(captureValue,{"version","stream","expressions"});
        if(integer(inverse->at("clip"))!=integer(packet.at("clip")) || integer(captureValue.at("version"))!=1 ||
           !captureValue.at("expressions").is_array())throw std::runtime_error("recorded note capture is malformed");
        if(!captureValue.at("stream").is_null())(void)parsePair(captureValue.at("stream"));
        for(const auto& pair:captureValue.at("expressions"))(void)parsePair(pair);
        return false;
    }
    if(inserted)closed(*inverse,{"clip","note"});
    else {
        closed(*inverse,{"clip","note","start","dur","key"},{"vel","velOff","chan","prob","cents"});
        NoteValues fields;fields.id=noteId(inverse->at("note"));fields.start=integer(inverse->at("start"));
        fields.duration=integer(inverse->at("dur"));fields.key=smallInteger(inverse->at("key"));
        fields.velocity=inverse->contains("vel")?smallInteger(inverse->at("vel")):100;
        fields.releaseVelocity=inverse->contains("velOff")?smallInteger(inverse->at("velOff")):64;
        fields.channel=inverse->contains("chan")?smallInteger(inverse->at("chan")):0;
        fields.probability=inverse->contains("prob")?smallInteger(inverse->at("prob")):10000;
        if(inverse->contains("cents")) {
            if(!inverse->at("cents").is_number())throw std::runtime_error("recorded note cents must be numeric");
            fields.cents=inverse->at("cents").get<double>();
        }
        if(!makeRecord(fields,{},error))throw std::runtime_error("invalid historical note inverse: "+error);
    }
    if(integer(inverse->at("clip"))!=integer(packet.at("clip")) || noteId(inverse->at("note"))!=noteId(packet.at("note")))
        throw std::runtime_error("recorded note inverse names a different clip or note");
    if(replay.asInverse && *inverse!=packet)throw std::runtime_error("historical inverse payload differs from recorded bytes");
    return true;
}
struct LegacyPlan {Plan rows;std::optional<NoteValues> before;};
LegacyPlan legacyPlan(OpContext& context,const Payload& packet,Legacy action) {
    switch(action) {
        case Legacy::Insert:closed(packet,{"clip","note","start","dur","key"},{"vel","velOff","chan","prob","cents"});break;
        case Legacy::Delete:closed(packet,{"clip","note"});break;
        case Legacy::Move:closed(packet,{"clip","note","start","key"});break;
        case Legacy::Resize:closed(packet,{"clip","note","dur"});break;
        case Legacy::Velocity:closed(packet,{"clip","note","vel"});break;
    }
    const auto clip=integer(packet.at("clip"));const auto id=noteId(packet.at("note"));
    const bool historicalScalar=historicalScalarReplay(context,packet,action);
    const auto expectedReplay=capturedReplayPlan(context,packet,action);
    auto captured=capture(context.db,clip);
    if(!captured.streamRow && action==Legacy::Insert) {
        if(expectedReplay && !expectedReplay->stream.before && expectedReplay->stream.after) {
            captured.creationRow=*expectedReplay->stream.after;(*captured.creationRow)["data"]=blob(nativeEmpty());
        } else captured.creationRow=nativeCreation(context.db,clip,historicalScalar);
    }
    const auto value=snapshot(std::move(captured));Diff diff;LegacyPlan result;
    if(action==Legacy::Insert) {
        NoteValues fields;fields.id=id;fields.start=integer(packet.at("start"));fields.duration=integer(packet.at("dur"));fields.key=smallInteger(packet.at("key"));
        fields.velocity=packet.contains("vel")?smallInteger(packet.at("vel")):100;
        fields.releaseVelocity=packet.contains("velOff")?smallInteger(packet.at("velOff")):64;
        fields.channel=packet.contains("chan")?smallInteger(packet.at("chan")):0;
        fields.probability=packet.contains("prob")?smallInteger(packet.at("prob")):10000;
        if(packet.contains("cents")) {
            if(!packet.at("cents").is_number())throw std::runtime_error("note cents must be numeric");
            fields.cents=packet.at("cents").get<double>();
        }
        std::string error;auto record=makeRecord(fields,{},error);if(!record)throw std::runtime_error(error);
        diff.added.push_back(std::move(*record));
    } else {
        const auto found=std::find_if(value.baseline.records.begin(),value.baseline.records.end(),[&](const auto& record){return recordValues(record).id==id;});
        if(found==value.baseline.records.end())throw std::runtime_error("no such note");
        auto fields=recordValues(*found);result.before=fields;
        if(action==Legacy::Delete)diff.removed.push_back(*found);
        else {
            if(action==Legacy::Move){fields.start=integer(packet.at("start"));fields.key=smallInteger(packet.at("key"));}
            else if(action==Legacy::Resize)fields.duration=integer(packet.at("dur"));
            else fields.velocity=smallInteger(packet.at("vel"));
            std::string error;auto changed=withValues(*found,fields,error);if(!changed)throw std::runtime_error(error);
            diff.changed.push_back({*found,std::move(*changed)});
        }
    }
    result.rows=fromDiff(context,value,diff,historicalScalar);
    if(expectedReplay && !samePlan(result.rows,*expectedReplay))throw std::runtime_error("stored note forward payload contradicts its captured inverse");
    return result;
}
bool legacyApply(OpContext& context,const Payload& packet,std::string& error,Legacy action) {
    try {
        if(packet.contains("note_capture")) {
            if(action==Legacy::Velocity)throw std::runtime_error("velocity inverse remains scalar");
            applyPlan(context,restorePlan(context,packet));
        } else applyPlan(context,legacyPlan(context,packet,action).rows);
        return true;
    } catch(const std::exception& exception){error=exception.what();return false;}
}
bool legacyInverse(OpContext& context,const Payload& packet,Payload& inverse,std::string& error,Legacy action) {
    try {
        if(packet.contains("note_capture")) {
            if(action==Legacy::Velocity)throw std::runtime_error("velocity inverse remains scalar");
            inverse=inverseCapture(restorePlan(context,packet));return true;
        }
        auto plan=legacyPlan(context,packet,action);
        if(action==Legacy::Velocity)inverse={{"clip",integer(packet.at("clip"))},{"note",noteId(packet.at("note"))},{"vel",plan.before->velocity}};
        else inverse=inverseCapture(plan.rows);
        return true;
    } catch(const std::exception& exception){error=exception.what();return false;}
}
bool apply(OpContext& context,const Payload& packet,std::string& error) {
    try {applyPlan(context,packet.contains("note_capture")?restorePlan(context,packet):checkedForwardPlan(context,packet));return true;}
    catch(const std::exception& exception){error=exception.what();return false;}
}
bool buildInverse(OpContext& context,const Payload& packet,Payload& inverse,std::string& error) {
    try {inverse=inverseCapture(packet.contains("note_capture")?restorePlan(context,packet):checkedForwardPlan(context,packet));return true;}
    catch(const std::exception& exception){error=exception.what();return false;}
}
constexpr Field fields[]={{"clip",FieldType::Int},{"stream_id",FieldType::Int},{"from",FieldType::Text},
    {"added",FieldType::Array},{"removed",FieldType::Array},{"changed",FieldType::Array},{"create",FieldType::Object,false}};
const OpDescriptor descriptors[]={{"note.applyDiff","Apply an explicit exact-record MIDI note diff",Scope::Edit,
    EngineImpact::Snapshot,fields,false,false,apply,buildInverse,""}};
} // namespace

Snapshot captureNotes(Store& store,std::int64_t clip,std::optional<std::int64_t> newStreamId) {
    try {
        Read read(store.db());auto result=capture(store.db(),clip);result.sourceFile=filename(store);result.head=OpJournal(store).headSeq();
        if(result.streamRow) {
            if(newStreamId && *newStreamId!=integer(result.streamRow->at("id")))throw std::runtime_error("existing canonical stream ID is authoritative");
        } else if(newStreamId) {
            if(adi::detail::captureRow(store.db(),"event_streams","id",*newStreamId))throw std::runtime_error("new notes stream identity is occupied");
            result.creationRow=edit::defaultRow(store.db(),"event_streams",{{"id",*newStreamId},{"clip_id",clip},
                {"stream_kind","notes"},{"channel",nullptr},{"controller",nullptr},{"data",blob(nativeEmpty())}});
        }
        return detail::makeSnapshot(std::move(result));
    } catch(const std::exception& exception){Snapshot result;result.error=exception.what();return result;}
}
CommitResult commitDiff(Store& store,const PreparedDiff& prepared,Actor actor) {
    CommitResult result;
    if(!prepared.ok){result.error=prepared.error;return result;}
    try {
        if(filename(store)!=prepared.sourceFile){result.error="project changed after note diff preparation";result.stale=true;return result;}
        if(prepared.requests.empty()) {
            // OpJournal's empty-batch fast path precedes its head guard. Keep
            // the prepared contract here without creating an empty journal op.
            Read read(store.db());const auto current=OpJournal(store).headSeq();
            if(prepared.options.expectHead && current!=*prepared.options.expectHead) {
                result.stale=true;result.headFound=current;result.error="stale: the undo head moved";return result;
            }
            result.ok=true;return result;
        }
        auto requests=prepared.requests;for(auto& request:requests)request.actor=actor;
        return OpJournal(store).commit(requests,prepared.options);
    } catch(const std::exception& exception){result.error=exception.what();return result;}
}
std::span<const OpDescriptor> diffOps(){return descriptors;}
bool noteInsertApply(OpContext& c,const Payload& p,std::string& e){return legacyApply(c,p,e,Legacy::Insert);}
bool noteInsertInverse(OpContext& c,const Payload& p,Payload& i,std::string& e){return legacyInverse(c,p,i,e,Legacy::Insert);}
bool noteDeleteApply(OpContext& c,const Payload& p,std::string& e){return legacyApply(c,p,e,Legacy::Delete);}
bool noteDeleteInverse(OpContext& c,const Payload& p,Payload& i,std::string& e){return legacyInverse(c,p,i,e,Legacy::Delete);}
bool noteMoveApply(OpContext& c,const Payload& p,std::string& e){return legacyApply(c,p,e,Legacy::Move);}
bool noteMoveInverse(OpContext& c,const Payload& p,Payload& i,std::string& e){return legacyInverse(c,p,i,e,Legacy::Move);}
bool noteResizeApply(OpContext& c,const Payload& p,std::string& e){return legacyApply(c,p,e,Legacy::Resize);}
bool noteResizeInverse(OpContext& c,const Payload& p,Payload& i,std::string& e){return legacyInverse(c,p,i,e,Legacy::Resize);}
bool noteVelocityApply(OpContext& c,const Payload& p,std::string& e){return legacyApply(c,p,e,Legacy::Velocity);}
bool noteVelocityInverse(OpContext& c,const Payload& p,Payload& i,std::string& e){return legacyInverse(c,p,i,e,Legacy::Velocity);}

namespace detail {
std::vector<Payload> expressionSet(const SnapshotData& snapshot,std::uint64_t id) {
    return selectedExpressions(snapshot.captured.expressionRows,id);
}
bool makeForwardRequest(const SnapshotData& snapshot,const StreamPlan& plan,OpRequest& output,std::string& error) {
    try {
        if(!plan.ok || !plan.changed)throw std::runtime_error("note request needs a changed valid stream plan");
        if(!snapshot.streamId && !snapshot.creationId)throw std::runtime_error("new notes stream needs a captured literal creation identity");
        Payload added=Payload::array(),removed=Payload::array(),changed=Payload::array();
        const auto expressionIndex=indexExpressions(snapshot.captured.expressionRows);
        for(const auto& record:plan.effects.added) {
            if(!expressionRows(expressionIndex,recordValues(record).id).empty())throw std::runtime_error("new note identity already owns orphan expression rows");
            added.push_back(recordPacket(record));
        }
        for(const auto& record:plan.effects.removed)removed.push_back({{"record",recordPacket(record)},
            {"expressions",expressionRows(expressionIndex,recordValues(record).id)}});
        for(const auto& change:plan.effects.changed)changed.push_back({{"old",recordPacket(change.oldRecord)},{"new",recordPacket(change.newRecord)}});
        OpRequest request;request.opType="note.applyDiff";request.label="Edit Notes";request.targetKind="clip";request.targetId=snapshot.clip;
        request.payload={{"clip",snapshot.clip},{"stream_id",snapshot.streamId?*snapshot.streamId:*snapshot.creationId},
            {"from",snapshot.from},{"added",std::move(added)},{"removed",std::move(removed)},{"changed",std::move(changed)}};
        if(!snapshot.streamId)request.payload["create"]=*snapshot.captured.creationRow;
        output=std::move(request);return true;
    } catch(const std::exception& exception){error=exception.what();return false;}
}
} // namespace detail
} // namespace adi::notes
