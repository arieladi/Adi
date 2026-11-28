#pragma once
// SC-320 (ADR-0258 d16): after a plug-in has brought ADI down while a project
// loaded, ADI offers to open that project with its plug-ins held off. Live
// lacks it; this is ADI's addition.
//
// Plug-ins run in ADI's own process (ADR-0047 d3), so a plug-in that crashes
// as it loads takes ADI with it, and every later open of the project would
// crash the same way. The open therefore leaves a marker on disk while its
// plug-ins load, naming the project and the plug-in being loaded; a clean end
// removes it. A marker still there at the next open of that project is the
// crash: the project then opens with its plug-ins held off -- each one a
// placeholder (ADR-0011) that keeps its state and parameter rows -- and the
// window asks whether to load all but the plug-in that was loading, all of
// them, or none. A held-off plug-in is not woken by a scan (SC-318); the next
// ordinary open loads it again.

#include "adi/engine/session.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace adi::ui {

/// What the marker says: the project, and the plug-in whose load had begun
/// and not ended (empty when the crash came between two loads).
struct LoadingMarker {
    std::string project;   ///< the project file, UTF-8
    std::string format, uid, name;
};

/// The marker file. Each write replaces the file whole; a crash mid-write
/// leaves either the old marker or the new one, never a torn one that would
/// hide the crash.
class LoadingGuard {
  public:
    explicit LoadingGuard(std::filesystem::path file) : file_(std::move(file)) {}
    /// The project's plug-ins are about to load.
    void arm(const std::filesystem::path &project);
    /// One plug-in's load begins; `ended` once it has returned, loaded or not.
    void loading(const std::string &format, const std::string &uid, const std::string &name);
    void ended();
    /// The loads are over: the marker goes.
    void disarm();
    [[nodiscard]] bool armed() const noexcept { return armed_; }
    [[nodiscard]] const std::filesystem::path &file() const noexcept { return file_; }
    /// The marker a crashed open left, if any.
    [[nodiscard]] static std::optional<LoadingMarker> read(const std::filesystem::path &file);
    /// True when `marker` names `project` (the same file, however spelled).
    [[nodiscard]] static bool names(const LoadingMarker &marker, const std::filesystem::path &project);

  private:
    void write() const;
    std::filesystem::path file_;
    LoadingMarker marker_;
    bool armed_ = false;
};

/// Which plug-ins the loader holds off. Built-in devices always load.
struct PluginHoldOff {
    enum class Scope { None, One, All };
    Scope scope = Scope::None;
    std::string format, uid;   ///< the one, for Scope::One
    [[nodiscard]] bool holds(const std::string &refFormat, const std::string &refUid) const noexcept;
};

/// `inner`, with every plug-in load written to `guard` while it is armed and
/// the plug-ins `holdOff` names refused (a placeholder, its reason said).
[[nodiscard]] engine::DeviceLoader holdingOff(engine::DeviceLoader inner,
                                              std::shared_ptr<const PluginHoldOff> holdOff,
                                              std::shared_ptr<LoadingGuard> guard);

/// Where the app keeps its marker: beside its settings.
[[nodiscard]] std::filesystem::path loadingMarkerFile();

} // namespace adi::ui
