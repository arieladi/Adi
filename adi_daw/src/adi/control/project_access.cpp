// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/project_access.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <utility>

namespace adi::control {

std::optional<Tier> readProjectAgentTier(const Store& store) {
    if (!store.db().tableExists("session_state")) return std::nullopt;
    SQLite::Statement row(const_cast<SQLite::Database&>(store.db()),
                          "SELECT value FROM session_state WHERE key='agent.tier'");
    if (!row.executeStep()) return std::nullopt;
    const auto value = Json::parse(row.getColumn(0).getString(), nullptr, false);
    if (!value.is_string()) return Tier::Observe;
    return tierFromString(value.get_ref<const std::string&>()).value_or(Tier::Observe);
}

bool setProjectAgentTier(Store& store, std::optional<Tier> tier) {
    if (store.readOnly()) return false;
    if (!tier) {
        store.db().exec("DELETE FROM session_state WHERE key='agent.tier'");
    } else {
        SQLite::Statement row(store.db(),
            "INSERT INTO session_state(key,value) VALUES('agent.tier',?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        row.bind(1, Json(toString(*tier)).dump());
        row.exec();
    }
    return true;
}

void ProjectSet::add(std::shared_ptr<ProjectAccess> project) {
    if (!project) return;
    std::lock_guard lock(mutex_);
    projects_.push_back(std::move(project));
}

void ProjectSet::clear() {
    std::lock_guard lock(mutex_);
    projects_.clear();
}

std::shared_ptr<ProjectAccess> ProjectSet::find(std::string_view handle) const {
    std::lock_guard lock(mutex_);
    for (const auto& p : projects_)
        if (p->handle() == handle) return p;
    return nullptr;
}

void ProjectSet::allowImplicitSingle(bool allow) {
    std::lock_guard lock(mutex_);
    implicitSingle_ = allow;
}

std::vector<std::shared_ptr<ProjectAccess>> ProjectSet::all() const {
    std::lock_guard lock(mutex_);
    return projects_;
}

bool ProjectSet::contains(const std::shared_ptr<ProjectAccess>& project) const {
    std::lock_guard lock(mutex_);
    return std::find(projects_.begin(), projects_.end(), project) != projects_.end();
}

std::shared_ptr<ProjectAccess> ProjectSet::resolve(
    const Json& params, RpcError& error, const std::shared_ptr<const ProjectAccess>& boundProject) const {
    const auto it = params.is_object() ? params.find("project") : params.end();
    const bool named = params.is_object() && it != params.end() && !it->is_null();
    std::lock_guard lock(mutex_);
    const auto matchesBinding = [&boundProject](const std::shared_ptr<ProjectAccess>& project) {
        return !boundProject || project == boundProject;
    };
    if (!named) {
        if (implicitSingle_ && projects_.size() == 1 && matchesBinding(projects_.front()))
            return projects_.front();
        error = {errc::InvalidParams,
                 projects_.empty() ? "no project is open"
                 : projects_.size() > 1 ? "several projects are open: name one (project.list)"
                                        : "name the project (project.list)",
                 nullptr};
        return nullptr;
    }
    if (!it->is_string()) {
        error = {errc::InvalidParams, "\"project\" is a project handle (a string)", nullptr};
        return nullptr;
    }
    const auto& wanted = it->get_ref<const std::string&>();
    for (const auto& p : projects_)
        if (p->handle() == wanted && matchesBinding(p)) return p;
    // AC-17: a request naming a project that is not bound touches nothing.
    error = {errc::InvalidParams, boundProject ? "the run's original project is no longer open"
                                             : "no open project has that handle (project.list)", nullptr};
    return nullptr;
}

}  // namespace adi::control
