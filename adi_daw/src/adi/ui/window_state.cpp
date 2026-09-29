// SPDX-License-Identifier: GPL-3.0-or-later
#include "window_state.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cmath>
#include <nlohmann/json.hpp>
namespace adi::ui {
WindowState ViewStateStore::load(const std::string &window, DesktopDefaults defaults) const {
    WindowState state(defaults);
    SQLite::Statement query(
        store_.db(),
        "SELECT value FROM ui_view WHERE scope_kind='project' AND scope_id IS NULL AND key=?");
    query.bind(1, "shell." + window);
    if (!query.executeStep())
        return state;
    try {
        const auto j = nlohmann::json::parse(query.getColumn(0).getString());
        if (j.at("version") != 1)
            return state;
        const auto &slots = j.at("panels");
        if (slots.size() != 2)
            return state;
        auto panel = [&](std::size_t i) {
            const auto id = slots.at(i).at("id").get<int>();
            if (id < 0 || id > 1)
                throw std::runtime_error("panel id");
            const auto width = slots.at(i).at("width").get<int>();
            if (width < (id == 0 ? 120 : 160) || width > 10000)
                throw std::runtime_error("width");
            return PanelGeometry{static_cast<Panel>(id), width, id == 0 ? 120 : 160,
                                 slots.at(i).at("collapsed").get<bool>()};
        };
        auto a = panel(0), b = panel(1);
        if (a.panel == b.panel)
            return state;
        const double zoom = j.at("zoom").get<double>();
        const int height = j.at("deviceHeight").get<int>();
        if (!std::isfinite(zoom) || zoom < .25 || zoom > 8 || height < 0 || height > 4000)
            return state;
        state.panels = PanelLayout(a, b, 240);
        state.zoom = zoom;
        state.deviceHeight = height;
        state.docked = j.at("docked").get<bool>();
        return state;
    } catch (const std::exception &) {
        return WindowState(defaults);
    }
}
bool ViewStateStore::save(const std::string &window, const WindowState &state, std::string &error) {
    try {
        if (store_.readOnly()) {
            error = "Project is read-only";
            return false;
        }
        if (!std::isfinite(state.zoom) || state.zoom < .25 || state.zoom > 8 ||
            state.deviceHeight < 0 || state.deviceHeight > 4000) {
            error = "Invalid view geometry";
            return false;
        }
        for (const auto &p : state.panels.slots())
            if (p.width < p.minWidth || p.width > 10000) {
                error = "Invalid panel width";
                return false;
            }
        nlohmann::json j = {{"version", 1},
                            {"zoom", state.zoom},
                            {"deviceHeight", state.deviceHeight},
                            {"docked", state.docked},
                            {"panels", nlohmann::json::array()}};
        for (const auto &p : state.panels.slots())
            j["panels"].push_back({{"id", static_cast<int>(p.panel)},
                                   {"width", p.width},
                                   {"collapsed", p.collapsed}});
        SQLite::Transaction transaction(store_.db());
        SQLite::Statement st(store_.db(),
                             "INSERT INTO ui_view(scope_kind,scope_id,key,value) VALUES "
                             "('project',NULL,?,?) ON CONFLICT DO UPDATE SET value=excluded.value");
        st.bind(1, "shell." + window);
        st.bind(2, j.dump());
        st.exec();
        transaction.commit();
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
} // namespace adi::ui
