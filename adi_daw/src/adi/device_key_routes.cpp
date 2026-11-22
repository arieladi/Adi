// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/device_key_routes.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <stdexcept>
namespace adi {
void writeRoutingKeyTap(SQLite::Database &db, std::int64_t id, const Payload &tap) {
    SQLite::Statement target(db,
                             "SELECT 1 FROM routing r JOIN tracks t ON t.id=r.src_id "
                             "JOIN devices d ON d.id=r.dst_id WHERE r.id=? AND r.kind='sidechain' "
                             "AND r.src_kind='track' AND r.dst_kind='device' AND d.is_rack=0");
    target.bind(1, id);
    if (!target.executeStep())
        throw std::runtime_error("key tap needs a track-to-device sidechain route");
    if (tap.is_null()) {
        SQLite::Statement erase(db, "DELETE FROM routing_key_taps WHERE routing_id=?");
        erase.bind(1, id);
        erase.exec();
        return;
    }
    if (!tap.is_string() || (tap != "pre_fx" && tap != "post_fx" && tap != "post_mixer"))
        throw std::runtime_error("key tap must be pre_fx, post_fx or post_mixer");
    SQLite::Statement set(db, "INSERT INTO routing_key_taps(routing_id,tap) VALUES(?,?) "
                              "ON CONFLICT(routing_id) DO UPDATE SET tap=excluded.tap");
    set.bind(1, id);
    set.bind(2, tap.get<std::string>());
    set.exec();
}
namespace {
bool apply(OpContext &context, const Payload &payload, std::string &error) {
    try {
        writeRoutingKeyTap(context.db, payload.at("id").get<std::int64_t>(),
                           payload.value("tap", Payload{}));
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool inverse(OpContext &context, const Payload &payload, Payload &undo, std::string &error) {
    try {
        const auto id = payload.at("id").get<std::int64_t>();
        SQLite::Statement q(context.db, "SELECT tap FROM routing_key_taps WHERE routing_id=?");
        q.bind(1, id);
        undo = {{"id", id},
                {"tap", q.executeStep() ? Payload(q.getColumn(0).getString()) : Payload{}}};
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
constexpr std::string_view taps[]{"pre_fx", "post_fx", "post_mixer"};
constexpr std::array<Field, 2> fields{
    {{"id", FieldType::Int, true, "sidechain routing row"},
     {"tap", FieldType::Text, false, "absent means Post Mixer", taps}}};
const std::array<OpDescriptor, 1> descriptors{
    {{"routing.setKeyTap", "Choose a device key's source tap", Scope::Edit,
      EngineImpact::GraphRebuild, fields, false, false, apply, inverse, ""}}};
} // namespace
std::span<const OpDescriptor> deviceKeyRouteOps() { return descriptors; }
} // namespace adi
