// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/project_access.hpp"

#include <utility>

namespace adi::control {

void ProjectSet::add(std::shared_ptr<ProjectAccess> project) {
    if (!project) return;
    std::lock_guard lock(mutex_);
    projects_.push_back(std::move(project));
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

std::shared_ptr<ProjectAccess> ProjectSet::resolve(const Json& params, RpcError& error) const {
    const auto it = params.is_object() ? params.find("project") : params.end();
    const bool named = params.is_object() && it != params.end() && !it->is_null();
    std::lock_guard lock(mutex_);
    if (!named) {
        if (implicitSingle_ && projects_.size() == 1) return projects_.front();
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
        if (p->handle() == wanted) return p;
    // AC-17: a request naming a project that is not bound touches nothing.
    error = {errc::InvalidParams, "no open project has that handle (project.list)", nullptr};
    return nullptr;
}

}  // namespace adi::control
