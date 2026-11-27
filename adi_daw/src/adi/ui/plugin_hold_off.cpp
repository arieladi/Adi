#include "adi/ui/plugin_hold_off.hpp"

#include "adi/appdata.hpp"
#include "adi/media/media_ops.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>
#include <utility>

namespace adi::ui {
namespace fs = std::filesystem;

void LoadingGuard::arm(const fs::path &project) {
    marker_ = {};
    marker_.project = media::pathUtf8(project);
    armed_ = true;
    write();
}

void LoadingGuard::loading(const std::string &format, const std::string &uid, const std::string &name) {
    if (!armed_)
        return;
    marker_.format = format;
    marker_.uid = uid;
    marker_.name = name;
    write();
}

void LoadingGuard::ended() {
    if (!armed_ || (marker_.format.empty() && marker_.uid.empty()))
        return;
    marker_.format.clear();
    marker_.uid.clear();
    marker_.name.clear();
    write();
}

void LoadingGuard::disarm() {
    armed_ = false;
    marker_ = {};
    if (file_.empty())
        return;
    std::error_code ec;
    fs::remove(file_, ec);
}

void LoadingGuard::write() const {
    if (file_.empty())
        return;
    const nlohmann::json j = {{"project", marker_.project},
                              {"format", marker_.format},
                              {"uid", marker_.uid},
                              {"name", marker_.name}};
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    // Written beside and renamed over: a crash during the write keeps the earlier marker whole.
    fs::path next = file_;
    next += ".next";
    {
        std::ofstream out(next, std::ios::binary | std::ios::trunc);
        if (!out)
            return;
        out << j.dump();
        if (!out.flush())
            return;
    }
    fs::rename(next, file_, ec);
}

std::optional<LoadingMarker> LoadingGuard::read(const fs::path &file) {
    if (file.empty())
        return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    const auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object() || !j.contains("project") || !j["project"].is_string())
        return std::nullopt;
    LoadingMarker m;
    m.project = j["project"].get<std::string>();
    const auto text = [&](const char *key) {
        const auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    m.format = text("format");
    m.uid = text("uid");
    m.name = text("name");
    if (m.project.empty())
        return std::nullopt;
    return m;
}

bool LoadingGuard::names(const LoadingMarker &marker, const fs::path &project) {
    const fs::path named = media::pathFromUtf8(marker.project);
    std::error_code ec;
    if (fs::equivalent(named, project, ec))
        return true;
    return named.lexically_normal() == project.lexically_normal();
}

bool PluginHoldOff::holds(const std::string &refFormat, const std::string &refUid) const noexcept {
    if (refFormat == "internal")
        return false; // ADI's own devices are not plug-ins
    switch (scope) {
    case Scope::None:
        return false;
    case Scope::All:
        return true;
    case Scope::One:
        return refFormat == format && refUid == uid;
    }
    return false;
}

engine::DeviceLoader holdingOff(engine::DeviceLoader inner, std::shared_ptr<const PluginHoldOff> holdOff,
                                std::shared_ptr<LoadingGuard> guard) {
    if (!inner)
        return inner;
    return [inner = std::move(inner), holdOff = std::move(holdOff), guard = std::move(guard)](
               const engine::DeviceRequest &rq, std::string &error) -> std::unique_ptr<device::DeviceInstance> {
        if (rq.ref != nullptr && holdOff && holdOff->holds(rq.ref->format, rq.ref->uid)) {
            error = holdOff->scope == PluginHoldOff::Scope::All
                        ? "held off: ADI closed while this project's plug-ins were loading"
                        : "held off: ADI closed while this plug-in was loading";
            return nullptr;
        }
        const bool plugin = rq.ref != nullptr && rq.ref->format != "internal";
        if (plugin && guard)
            guard->loading(rq.ref->format, rq.ref->uid,
                           !rq.ref->name.empty() ? rq.ref->name
                           : rq.device != nullptr ? rq.device->name
                                                  : rq.ref->uid);
        auto made = inner(rq, error);
        if (plugin && guard)
            guard->ended();
        return made;
    };
}

fs::path loadingMarkerFile() {
    const auto paths = appdata::pathsFor(appdata::App::Daw);
    return paths.config.empty() ? fs::path() : paths.config / "loading-project.json";
}

} // namespace adi::ui
