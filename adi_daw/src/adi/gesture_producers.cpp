// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/gesture_producers.hpp"
#include "adi/op_transaction_resources_detail.hpp"
#include "adi/row_capture.hpp"
#include "adi/automation_edits.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace adi::gesture {
namespace {
using adi::detail::signedInteger;
using adi::detail::sameSqlValue;
struct Captured final : Origin {
    std::map<std::string, std::optional<Payload>> rows;
    std::vector<Payload> tracks;
    std::vector<TargetClaim> claims;
};
const Captured& captured(const Origin& origin) {
    const auto* value = dynamic_cast<const Captured*>(&origin);
    if (!value) throw std::runtime_error("gesture producer origin type differs");
    return *value;
}
std::optional<Payload> row(const Store& store, const std::string& table, const char* key, std::int64_t id) {
    return adi::detail::captureRow(const_cast<SQLite::Database&>(store.db()), table, key, id);
}
Payload required(const Store& store, const std::string& table, const char* key, std::int64_t id) {
    auto result = row(store, table, key, id);
    if (!result) throw std::runtime_error("gesture target missing: " + table);
    return std::move(*result);
}
OpRequest request(std::string type, Payload payload) {
    OpRequest result; result.opType = std::move(type); result.payload = std::move(payload); return result;
}
void guard(Proposal& proposal, const Store& store, const std::string& table, const char* key, std::int64_t id) {
    proposal.rowGuards.push_back({table, {{key, id}}, row(store, table, key, id)});
}
void setGuard(Proposal& proposal, const Store& store, const std::string& table, Payload selector = Payload::object()) {
    proposal.rowSetGuards.push_back({table, selector, adi::detail::captureGuardRows(store, table, selector)});
}
void identity(const Payload& before, const Payload& current, std::initializer_list<const char*> keys) {
    for (const auto* key : keys)
        if (!before.contains(key) || !current.contains(key) || !sameSqlValue(before.at(key), current.at(key)))
            throw std::runtime_error("gesture target identity changed");
}
void uniqueIds(std::vector<std::int64_t>& ids) {
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) throw std::runtime_error("duplicate gesture target");
}
TargetClaim claim(TargetDomain domain, std::int64_t id, std::string endpoint) {
    return {ClaimKind::ScalarEndpoint, domain, id, std::move(endpoint), {}};
}
class StandardProducer : public Producer {
public:
    std::vector<TargetClaim> targetClaims(const Origin& origin) const override { return captured(origin).claims; }
};

struct ScalarDefinition { const char* table; const char* key; const char* column; const char* operation; const char* field; };
ScalarDefinition definition(ScalarKind kind) {
    switch (kind) {
        case ScalarKind::TrackVolume: return {"mixer_strip","track_id","volume_db","mixer.setVolume","db"};
        case ScalarKind::TrackPan: return {"mixer_strip","track_id","pan","mixer.setPan","pan"};
        case ScalarKind::TrackPanLeft: return {"mixer_pan_settings","track_id","pan_left","mixer.setPanLeft","pan"};
        case ScalarKind::TrackPanRight: return {"mixer_pan_settings","track_id","pan_right","mixer.setPanRight","pan"};
        case ScalarKind::TrackInputGain: return {"mixer_strip","track_id","input_gain_db","mixer.setInputGain","db"};
        case ScalarKind::RoutingGain: return {"routing","id","gain_db","routing.setGain","db"};
        case ScalarKind::RoutingPan: return {"routing","id","pan","routing.setPan","pan"};
        case ScalarKind::GroupSummingDrive: return {"group_summing","track_id","drive_db","group.setSummingDrive","db"};
    }
    throw std::runtime_error("unsupported scalar gesture");
}
class ScalarProducer final : public StandardProducer {
public:
    explicit ScalarProducer(ScalarTarget target) : target_(target) {}
    OriginResult captureOrigin(const Store& store, const Base&) override {
        try {
            const auto def = definition(target_.kind); auto result = std::make_unique<Captured>();
            const bool routing = target_.kind == ScalarKind::RoutingGain || target_.kind == ScalarKind::RoutingPan;
            result->rows["root"] = required(store, routing ? "routing" : "tracks", "id", target_.id);
            result->rows["value"] = row(store, def.table, def.key, target_.id);
            const bool split = target_.kind == ScalarKind::TrackPanLeft || target_.kind == ScalarKind::TrackPanRight;
            if (!result->rows.at("value") && !split) throw std::runtime_error("scalar gesture needs an existing control row");
            if (routing && result->rows.at("root")->at("kind") != "send") throw std::runtime_error("live routing gesture needs a send");
            result->claims.push_back(claim(routing ? TargetDomain::Routing : TargetDomain::Track, target_.id, def.operation));
            return {{}, std::move(result)};
        } catch (const std::exception& exception) { return {{Error::TargetConflict, exception.what()}, {}}; }
    }
    ProposalResult prepare(const Store& store, const Base&, const Origin& origin, const Input& input) const override {
        const auto* value = dynamic_cast<const ScalarInput*>(&input);
        if (!value || !std::isfinite(value->value)) return {{Error::InvalidInput, "scalar input must be finite"}, {}};
        try {
            const auto& before = captured(origin); const auto def = definition(target_.kind); Proposal result;
            const bool routing = target_.kind == ScalarKind::RoutingGain || target_.kind == ScalarKind::RoutingPan;
            const auto root = required(store, routing ? "routing" : "tracks", "id", target_.id);
            if (routing) identity(*before.rows.at("root"), root, {"id","src_kind","src_id","dst_kind","dst_id","kind","pre_fader"});
            else identity(*before.rows.at("root"), root, {"id","kind"});
            guard(result, store, routing ? "routing" : "tracks", "id", target_.id);
            guard(result, store, def.table, def.key, target_.id);
            const auto current = row(store, def.table, def.key, target_.id);
            double existing = 0;
            if (current) existing = current->at(def.column).get<double>();
            else if (target_.kind == ScalarKind::TrackPanLeft) existing = -1;
            else if (target_.kind == ScalarKind::TrackPanRight) existing = 1;
            else throw std::runtime_error("scalar control row was removed");
            if (existing != value->value)
                result.requests.push_back(request(def.operation, {{"id", target_.id}, {def.field, value->value}}));
            return {{}, std::move(result)};
        } catch (const std::exception& exception) { return {{Error::TargetConflict, exception.what()}, {}}; }
    }
private: ScalarTarget target_;
};

struct RelativeCaptured final : Origin {
    struct Entry {
        ScalarTarget target;
        std::unique_ptr<const Origin> origin;
        double native = 0, coordinate = 0;
    };
    ScalarControlDomain domain;
    std::vector<Entry> entries;
    std::vector<TargetClaim> claims;
    std::size_t driver = 0;
};
void relativeDomain(ScalarKind kind, const ScalarControlDomain &domain) {
    (void)definition(kind);
    if (!std::isfinite(domain.minimum) || !std::isfinite(domain.maximum) ||
        domain.minimum >= domain.maximum || !std::isfinite(domain.maximum - domain.minimum))
        throw std::runtime_error("relative scalar needs a finite positive control range");
    const bool gain = kind == ScalarKind::TrackVolume || kind == ScalarKind::TrackInputGain ||
                      kind == ScalarKind::RoutingGain;
    if (domain.silence &&
        (!gain || !std::isfinite(*domain.silence) || *domain.silence >= domain.minimum))
        throw std::runtime_error("relative scalar silence must be below a gain control's floor");
    const bool pan = kind == ScalarKind::TrackPan || kind == ScalarKind::TrackPanLeft ||
                     kind == ScalarKind::TrackPanRight || kind == ScalarKind::RoutingPan;
    if (pan && (domain.minimum < -1 || domain.maximum > 1))
        throw std::runtime_error("relative pan domain exceeds native bounds");
    if (kind == ScalarKind::GroupSummingDrive && (domain.minimum < -12 || domain.maximum > 24))
        throw std::runtime_error("relative summing drive domain exceeds native bounds");
}
double shifted(double from, double change, const ScalarControlDomain &domain) {
    // Compare distances before adding: even finite caller-declared domains
    // near the double ceiling cannot overflow an intermediate candidate.
    if (change >= 0 && change >= domain.maximum - from)
        return domain.maximum;
    if (change < 0 && -change >= from - domain.minimum)
        return domain.minimum;
    return std::clamp(from + change, domain.minimum, domain.maximum);
}
void appendProposal(Proposal &to, Proposal &&from) {
    for (auto &request : from.requests)
        to.requests.push_back(std::move(request));
    for (auto &resource : from.stateBlobs)
        to.stateBlobs.push_back(std::move(resource));
    for (auto &row : from.rowGuards)
        to.rowGuards.push_back(std::move(row));
    for (auto &rows : from.rowSetGuards)
        to.rowSetGuards.push_back(std::move(rows));
    if (from.presentationImage) {
        if (to.presentationImage && to.presentationImage->image != from.presentationImage->image)
            throw std::runtime_error("relative scalar child presentation images differ");
        to.presentationImage = std::move(from.presentationImage);
    }
}
class RelativeScalarProducer final : public Producer {
  public:
    explicit RelativeScalarProducer(RelativeScalarSelection selection)
        : selection_(std::move(selection)) {}
    OriginResult captureOrigin(const Store &store, const Base &base) override {
        try {
            relativeDomain(selection_.kind, selection_.domain);
            auto selected = selection_.selected;
            uniqueIds(selected);
            const bool routing = selection_.kind == ScalarKind::RoutingGain ||
                                 selection_.kind == ScalarKind::RoutingPan;
            // Validate the literal selection, even when the unselected driver
            // acts alone. It is never read again from mutable window state.
            for (const auto id : selected)
                (void)required(store, routing ? "routing" : "tracks", "id", id);
            const auto targets =
                std::binary_search(selected.begin(), selected.end(), selection_.driver)
                    ? selected
                    : std::vector<std::int64_t>{selection_.driver};
            auto result = std::make_unique<RelativeCaptured>();
            result->domain = selection_.domain;
            const auto def = definition(selection_.kind);
            bool driver = false;
            for (const auto id : targets) {
                const ScalarTarget target{selection_.kind, id};
                ScalarProducer child(target);
                auto origin = child.captureOrigin(store, base);
                if (!origin.status.ok())
                    return {std::move(origin.status), {}};
                const auto &value = captured(*origin.value).rows.at("value");
                const double native = value ? value->at(def.column).get<double>()
                                      : selection_.kind == ScalarKind::TrackPanLeft ? -1
                                                                                    : 1;
                if (!std::isfinite(native))
                    throw std::runtime_error("relative scalar origin must be finite");
                const auto claims = child.targetClaims(*origin.value);
                result->claims.insert(result->claims.end(), claims.begin(), claims.end());
                if (id == selection_.driver) {
                    result->driver = result->entries.size();
                    driver = true;
                }
                result->entries.push_back(
                    {target, std::move(origin.value), native,
                     std::clamp(native, result->domain.minimum, result->domain.maximum)});
            }
            if (!driver)
                throw std::runtime_error("relative scalar driver was not captured");
            return {{}, std::move(result)};
        } catch (const std::exception &error) {
            return {{Error::TargetConflict, error.what()}, {}};
        }
    }
    std::vector<TargetClaim> targetClaims(const Origin &origin) const override {
        const auto *before = dynamic_cast<const RelativeCaptured *>(&origin);
        if (!before)
            throw std::runtime_error("relative scalar origin type differs");
        return before->claims;
    }
    ProposalResult prepare(const Store &store, const Base &base, const Origin &origin,
                           const Input &input) const override {
        const auto *before = dynamic_cast<const RelativeCaptured *>(&origin);
        const auto *value = dynamic_cast<const RelativeScalarInput *>(&input);
        if (!before || !value || !std::isfinite(value->value))
            return {{Error::InvalidInput, "relative scalar input or origin type differs"}, {}};
        const auto &domain = before->domain;
        if (value->value < domain.minimum || value->value > domain.maximum ||
            before->driver >= before->entries.size())
            return {
                {Error::InvalidInput, "relative scalar coordinate is outside its captured domain"},
                {}};
        const double change = value->value - before->entries[before->driver].coordinate;
        try {
            Proposal result;
            for (std::size_t i = 0; i < before->entries.size(); ++i) {
                const auto &entry = before->entries[i];
                double wanted = entry.native;
                // Returning to Begin restores its exact raw native value, even
                // when its display coordinate was clamped. Other floored gains
                // stay parked; moving the driver itself may leave silence.
                if (change != 0 &&
                    !(i != before->driver && domain.silence && entry.native <= domain.minimum)) {
                    const auto position = i == before->driver
                                              ? value->value
                                              : shifted(entry.coordinate, change, domain);
                    wanted =
                        domain.silence && position == domain.minimum ? *domain.silence : position;
                }
                ScalarProducer child(entry.target);
                ScalarInput absolute(wanted);
                auto prepared = child.prepare(store, base, *entry.origin, absolute);
                if (!prepared.status.ok())
                    return {std::move(prepared.status), {}};
                appendProposal(result, std::move(prepared.value));
            }
            return {{}, std::move(result)};
        } catch (const std::exception &error) {
            return {{Error::TargetConflict, error.what()}, {}};
        }
    }

  private:
    RelativeScalarSelection selection_;
};
class TrackSwitchProducer final : public StandardProducer {
public:
    explicit TrackSwitchProducer(TrackSwitchTarget target) : target_(std::move(target)) {}
    OriginResult captureOrigin(const Store& store, const Base&) override {
        try {
            uniqueIds(target_.selected); auto result = std::make_unique<Captured>();
            result->tracks = adi::detail::captureGuardRows(store, "tracks", Payload::object());
            const auto clicked = required(store, "tracks", "id", target_.clicked);
            if (clicked.at("kind") == "master") throw std::runtime_error("Main has no track switch");
            result->rows["clicked"] = clicked;
            for (const auto id : target_.selected) (void)required(store, "tracks", "id", id);
            const auto targets = affected(*result);
            for (const auto id : targets) result->claims.push_back(claim(TargetDomain::Track, id,
                target_.kind == TrackSwitchKind::Solo ? "track.setSolo" : "track.setMute"));
            return {{}, std::move(result)};
        } catch (const std::exception& exception) { return {{Error::TargetConflict, exception.what()}, {}}; }
    }
    ProposalResult prepare(const Store& store, const Base&, const Origin& origin, const Input& input) const override {
        const auto* value = dynamic_cast<const TrackSwitchInput*>(&input);
        if (!value) return {{Error::InvalidInput, "track switch input type differs"}, {}};
        try {
            const auto& before = captured(origin); Proposal result;
            const auto current = adi::detail::captureGuardRows(store, "tracks", Payload::object());
            if (current.size() != before.tracks.size()) throw std::runtime_error("track switch membership changed");
            for (std::size_t i = 0; i < current.size(); ++i) identity(before.tracks[i], current[i], {"id","kind"});
            result.rowSetGuards.push_back({"tracks", Payload::object(), current});
            const bool solo = target_.kind == TrackSwitchKind::Solo;
            const bool original = solo ? before.rows.at("clicked")->at("soloed").get<std::int64_t>() != 0
                                       : before.rows.at("clicked")->at("muted").get<std::int64_t>() == 0;
            const bool on = value->value == TrackSwitchValue::ToggleFromOrigin ? !original : value->value == TrackSwitchValue::On;
            const bool together = target_.selected.size() > 1 && std::binary_search(target_.selected.begin(), target_.selected.end(), target_.clicked);
            for (const auto& track : current) {
                if (track.at("kind") == "master") continue;
                const auto id = signedInteger(track.at("id"));
                const bool acted = together ? std::binary_search(target_.selected.begin(), target_.selected.end(), id) : id == target_.clicked;
                if (!acted && !(solo && on && target_.exclusiveSolo && !target_.additiveSolo)) continue;
                const bool desired = solo ? acted && on : !on;
                const auto* column = solo ? "soloed" : "muted";
                if ((signedInteger(track.at(column)) != 0) != desired)
                    result.requests.push_back(request(solo ? "track.setSolo" : "track.setMute", {{"id", id}, {column, desired}}));
            }
            return {{}, std::move(result)};
        } catch (const std::exception& exception) { return {{Error::TargetConflict, exception.what()}, {}}; }
    }
private:
    std::vector<std::int64_t> affected(const Captured& origin) const {
        std::vector<std::int64_t> result;
        const bool together = target_.selected.size() > 1 && std::binary_search(target_.selected.begin(), target_.selected.end(), target_.clicked);
        for (const auto& track : origin.tracks) {
            const auto id = signedInteger(track.at("id"));
            if (track.at("kind") != "master" &&
                ((target_.kind == TrackSwitchKind::Solo && target_.exclusiveSolo && !target_.additiveSolo) ||
                 (together ? std::binary_search(target_.selected.begin(), target_.selected.end(), id) : id == target_.clicked))) result.push_back(id);
        }
        return result;
    }
    TrackSwitchTarget target_;
};

class BooleanProducer final : public StandardProducer {
public:
    BooleanProducer(TrackBooleanKind kind, std::vector<std::int64_t> ids) : kind_(kind), ids_(std::move(ids)) {}
    OriginResult captureOrigin(const Store& store, const Base&) override {
        try {
            uniqueIds(ids_); if (ids_.empty()) throw std::runtime_error("empty boolean gesture targets");
            auto result = std::make_unique<Captured>();
            for (const auto id : ids_) {
                auto item = required(store, "tracks", "id", id);
                if (item.at("kind") == "master") throw std::runtime_error("Main has no track switch");
                result->rows[std::to_string(id)] = std::move(item);
                result->claims.push_back(claim(TargetDomain::Track, id, kind_ == TrackBooleanKind::Solo ? "track.setSolo" : "track.setMute"));
            }
            return {{}, std::move(result)};
        } catch (const std::exception& exception) { return {{Error::TargetConflict, exception.what()}, {}}; }
    }
    ProposalResult prepare(const Store& store, const Base&, const Origin& origin, const Input& input) const override {
        const auto* values = dynamic_cast<const TrackBooleanInput*>(&input);
        if (!values || values->rows.size() != ids_.size()) return {{Error::InvalidInput, "boolean gesture target set differs"}, {}};
        try {
            std::map<std::int64_t, bool> desired;
            for (const auto& [id, value] : values->rows)
                if (!desired.emplace(id, value).second) throw std::runtime_error("duplicate boolean gesture target");
            Proposal result; const auto& before = captured(origin);
            for (const auto id : ids_) {
                if (!desired.contains(id)) throw std::runtime_error("boolean gesture target set differs");
                const auto current = required(store, "tracks", "id", id);
                identity(*before.rows.at(std::to_string(id)), current, {"id","kind"});
                guard(result, store, "tracks", "id", id);
                const auto* field = kind_ == TrackBooleanKind::Solo ? "soloed" : "muted";
                if ((signedInteger(current.at(field)) != 0) != desired.at(id))
                    result.requests.push_back(request(kind_ == TrackBooleanKind::Solo ? "track.setSolo" : "track.setMute", {{"id", id}, {field, desired.at(id)}}));
            }
            return {{}, std::move(result)};
        } catch (const std::exception& exception) { return {{Error::TargetConflict, exception.what()}, {}}; }
    }
private: TrackBooleanKind kind_; std::vector<std::int64_t> ids_;
};

class ClipLoopProducer final : public StandardProducer {
public:
    explicit ClipLoopProducer(std::int64_t id) : id_(id) {}
    OriginResult captureOrigin(const Store& store, const Base&) override {
        try {
            auto result = std::make_unique<Captured>(); result->rows["clip"] = required(store, "clips", "id", id_);
            result->claims.push_back({ClaimKind::Structural, TargetDomain::Material, id_, "clip.loop", {}});
            return {{}, std::move(result)};
        } catch (const std::exception& exception) { return {{Error::TargetConflict, exception.what()}, {}}; }
    }
    ProposalResult prepare(const Store& store, const Base&, const Origin& origin, const Input& input) const override {
        const auto* value = dynamic_cast<const ClipLoopInput*>(&input);
        if (!value) return {{Error::InvalidInput, "clip loop input type differs"}, {}};
        try {
            const auto current = required(store, "clips", "id", id_);
            identity(*captured(origin).rows.at("clip"), current, {"id","kind","time_base"});
            Proposal result; guard(result, store, "clips", "id", id_); guard(result, store, "clip_properties", "clip_id", id_);
            setGuard(result, store, "tempo_map");
            result.requests.push_back(request("clip.setLoop", {{"id", id_}, {"on", value->on}, {"start", value->startTicks}, {"length", value->lengthTicks}}));
            return {{}, std::move(result)};
        } catch (const std::exception& exception) { return {{Error::TargetConflict, exception.what()}, {}}; }
    }
private: std::int64_t id_;
};

void canonicalTempo(const Payload& value) {
    static const std::set<std::string> fields{"id","pos_ticks","bpm","curve","tension"};
    if (!value.is_object() || value.size() != fields.size()) throw std::runtime_error("tempo row has unsupported columns");
    for (const auto& field : fields) if (!value.contains(field)) throw std::runtime_error("tempo row has unsupported columns");
    for (const auto* field : {"id","pos_ticks","curve"}) (void)signedInteger(value.at(field));
    for (const auto* field : {"bpm","tension"})
        if (!value.at(field).is_number_float() || !std::isfinite(value.at(field).get<double>())) throw std::runtime_error("tempo row has unsupported stored types");
}
class TempoProducer final : public StandardProducer {
public:
    TempoProducer(std::int64_t position, bool insertion) : position_(position), insertion_(insertion) {}
    OriginResult captureOrigin(const Store& store, const Base&) override {
        try {
            auto result = std::make_unique<Captured>();
            const auto rows = adi::detail::captureGuardRows(store, "tempo_map", {{"pos_ticks", position_}});
            if (insertion_) { if (!rows.empty()) throw std::runtime_error("tempo insertion position is occupied"); }
            else { if (rows.size() != 1) throw std::runtime_error("tempo event is missing"); canonicalTempo(rows.front()); result->rows["tempo"] = rows.front(); }
            result->claims.push_back({ClaimKind::Structural, TargetDomain::Material, position_, "tempo", {}});
            return {{}, std::move(result)};
        } catch (const std::exception& exception) { return {{Error::TargetConflict, exception.what()}, {}}; }
    }
    ProposalResult prepare(const Store& store, const Base&, const Origin& origin, const Input& input) const override {
        const auto* existing = dynamic_cast<const TempoInput*>(&input);
        const auto* insertion = dynamic_cast<const TempoInsertInput*>(&input);
        if ((insertion_ && !insertion) || (!insertion_ && !existing)) return {{Error::InvalidInput, "tempo input type differs"}, {}};
        const double bpm = insertion_ ? insertion->bpm : existing->bpm;
        if (!std::isfinite(bpm) || bpm <= 0 || bpm >= 1000) return {{Error::InvalidInput, "tempo must be finite in (0,1000)"}, {}};
        try {
            Proposal result; setGuard(result, store, "tempo_map", {{"pos_ticks", position_}});
            const auto& rows = result.rowSetGuards.back().expectedRows;
            if (insertion_) {
                if (!rows.empty()) throw std::runtime_error("tempo insertion position became occupied");
                result.requests.push_back(request("project.insertTempoEvent", {{"pos", position_}, {"bpm", bpm}, {"curve", insertion->curve}}));
            } else {
                if (rows.size() != 1) throw std::runtime_error("tempo event is missing");
                const auto& current = rows.front(); canonicalTempo(current);
                identity(*captured(origin).rows.at("tempo"), current, {"id","pos_ticks","curve","tension"});
                if (current.at("bpm").get<double>() != bpm) {
                    result.requests.push_back(request("project.removeTempoEvent", {{"pos", position_}}));
                    result.requests.push_back(request("project.insertTempoEvent", {{"id", current.at("id")}, {"pos", position_},
                        {"bpm", bpm}, {"curve", current.at("curve")}, {"tension", current.at("tension")}}));
                }
            }
            return {{}, std::move(result)};
        } catch (const std::exception& exception) { return {{Error::TargetConflict, exception.what()}, {}}; }
    }
private: std::int64_t position_; bool insertion_;
};

class AutomationProducer final : public StandardProducer {
public:
    AutomationProducer(std::int64_t data, std::int64_t lane, std::optional<std::int64_t> clip, std::vector<std::uint64_t> ids)
        : data_(data), lane_(lane), clip_(clip), ids_(std::move(ids)) {}
    OriginResult captureOrigin(const Store& store, const Base&) override {
        try {
            std::sort(ids_.begin(), ids_.end());
            if (ids_.empty() || ids_.front() == 0 || std::adjacent_find(ids_.begin(), ids_.end()) != ids_.end()) throw std::runtime_error("invalid automation point selection");
            auto result = std::make_unique<Captured>(); result->rows["data"] = required(store, "automation_data", "id", data_);
            result->rows["lane"] = required(store, "automation_lanes", "id", lane_);
            if (clip_) result->rows["clip"] = required(store, "clips", "id", *clip_);
            result->claims.push_back({ClaimKind::Structural, TargetDomain::Material, data_, "automation", {}});
            return {{}, std::move(result)};
        } catch (const std::exception& exception) { return {{Error::TargetConflict, exception.what()}, {}}; }
    }
    ProposalResult prepare(const Store& store, const Base&, const Origin& origin, const Input& input) const override {
        const auto* values = dynamic_cast<const AutomationMoveInput*>(&input);
        if (!values || values->points.size() != ids_.size()) return {{Error::InvalidInput, "automation input selection differs"}, {}};
        try {
            const auto& before = captured(origin); const auto current = required(store, "automation_data", "id", data_);
            if (!sameSqlValue(*before.rows.at("data"), current)) throw std::runtime_error("automation stream changed");
            std::map<std::uint64_t, AutomationPointPosition> points;
            for (const auto& point : values->points)
                if (!std::isfinite(point.value) || !points.emplace(point.id, point).second) throw std::runtime_error("invalid automation points");
            Payload packet = Payload::array();
            for (const auto id : ids_) {
                if (!points.contains(id)) throw std::runtime_error("automation input selection differs");
                const auto& point = points.at(id); packet.push_back({{"id", id}, {"time", point.time}, {"value", point.value}});
            }
            Proposal result; guard(result, store, "automation_data", "id", data_); guard(result, store, "automation_lanes", "id", lane_);
            if (clip_) guard(result, store, "clips", "id", *clip_);
            const Payload clip = clip_ ? Payload(*clip_) : Payload(nullptr);
            setGuard(result, store, "automation_data", {{"lane_id", lane_}, {"clip_id", clip}});
            result.requests.push_back(automation::movePoints(data_, lane_, clip, std::move(packet)));
            return {{}, std::move(result)};
        } catch (const std::exception& exception) { return {{Error::TargetConflict, exception.what()}, {}}; }
    }
private: std::int64_t data_, lane_; std::optional<std::int64_t> clip_; std::vector<std::uint64_t> ids_;
};
} // namespace
std::unique_ptr<Producer> scalarProducer(ScalarTarget target) { return std::make_unique<ScalarProducer>(target); }
std::unique_ptr<Producer> relativeScalarProducer(RelativeScalarSelection selection) { return std::make_unique<RelativeScalarProducer>(std::move(selection)); }
std::unique_ptr<Producer> trackSwitchProducer(TrackSwitchTarget target) { return std::make_unique<TrackSwitchProducer>(std::move(target)); }
std::unique_ptr<Producer> trackBooleanProducer(TrackBooleanKind kind, std::vector<std::int64_t> targets) { return std::make_unique<BooleanProducer>(kind, std::move(targets)); }
std::unique_ptr<Producer> clipLoopProducer(std::int64_t clip) { return std::make_unique<ClipLoopProducer>(clip); }
std::unique_ptr<Producer> tempoProducer(std::int64_t position) { return std::make_unique<TempoProducer>(position, false); }
std::unique_ptr<Producer> tempoInsertProducer(std::int64_t position) { return std::make_unique<TempoProducer>(position, true); }
std::unique_ptr<Producer> automationMoveProducer(std::int64_t data, std::int64_t lane,
    std::optional<std::int64_t> clip, std::vector<std::uint64_t> points) {
    return std::make_unique<AutomationProducer>(data, lane, clip, std::move(points));
}
} // namespace adi::gesture
