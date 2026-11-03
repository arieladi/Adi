// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_state.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
namespace adi::ui {
namespace {
Payload toJson(const Selection &s) {
    Payload j{{"track", s.track}, {"clip", s.clip},     {"start", s.start},   {"end", s.end},
              {"insert", s.insert}, {"marked", s.marked}, {"toTrack", s.toTrack}};
    // The tracks picked with Ctrl/Cmd+click, so an undo puts them back with the rest; written only
    // when there are some.
    if (!s.picked.empty())
        j["picked"] = s.picked;
    return j;
}
Selection fromJson(const Payload &j) {
    Selection s;
    s.track = j.value("track", std::int64_t{0});
    s.clip = j.value("clip", std::int64_t{0});
    s.start = j.value("start", std::int64_t{0});
    s.end = j.value("end", std::int64_t{0});
    s.insert = j.value("insert", std::int64_t{0});
    s.marked = j.value("marked", false);
    s.toTrack = j.value("toTrack", std::int64_t{0}); // absent in a selection written before it
    if (const auto picked = j.find("picked"); picked != j.end() && picked->is_array())
        for (const auto &id : *picked)
            if (id.is_number_integer())
                s.picked.push_back(id.get<std::int64_t>());
    return s;
}
} // namespace
SessionState::SessionState(Store &store) : store_(store) {
    try {
        SQLite::Statement q(store_.db(), "SELECT value FROM session_state WHERE key='selection'");
        if (q.executeStep())
            selection_ = fromJson(Payload::parse(q.getColumn(0).getString()));
    } catch (const std::exception &) {
        selection_ = {}; // a malformed row is a lost selection, never a failed open
    }
}
bool SessionState::select(const Selection &s, std::string &error) {
    if (s == selection_)
        return true;
    selection_ = s;
    ++generation_;
    if (store_.readOnly())
        return true;
    try {
        SQLite::Statement st(store_.db(), "INSERT INTO session_state(key,value) VALUES"
                                          "('selection',?) ON CONFLICT DO UPDATE SET "
                                          "value=excluded.value");
        st.bind(1, toJson(s).dump());
        st.exec();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
Payload SessionState::advisory() const { return {{"selection", toJson(selection_)}}; }
void SessionState::restore(const Payload &blob) {
    if (!blob.is_object() || !blob.contains("selection"))
        return;
    std::string ignored;
    select(fromJson(blob.at("selection")), ignored);
}
} // namespace adi::ui
