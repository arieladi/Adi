// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace adi { class AppliedSource; }
namespace adi::device { class DeviceInstance; }
namespace adi::engine {
class Session;
class ProjectView;

/// Owner-thread registration. Retaining this value does not keep its owner
/// alive: destructor-entry revocation makes get() return null.
class SessionEndpoint {
public:
    [[nodiscard]] Session* get() const noexcept { return owner_; }
private:
    Session* owner_ = nullptr;
    friend class Session;
};
class ProjectViewEndpoint {
public:
    [[nodiscard]] ProjectView* get() const noexcept { return owner_; }
private:
    ProjectView* owner_ = nullptr;
    friend class ProjectView;
};

/// Session-issued instance identity, independent of a numeric device row ID.
/// Every token for the same actual instance shares its endpoint claim identity.
class DeviceRuntimeToken {
public:
    [[nodiscard]] bool sameInstance(const DeviceRuntimeToken&) const noexcept;
    [[nodiscard]] std::weak_ptr<const void> claimIdentity() const noexcept;
private:
    struct Data {
        std::weak_ptr<const SessionEndpoint> session;
        std::weak_ptr<const void> identity;
        device::DeviceInstance* instance = nullptr;
        std::int64_t deviceId = 0;
        std::uint64_t runtimeEpoch = 0, shapeEpoch = 0;
        std::optional<std::int64_t> pluginRef;
        std::string format, uid;
    };
    std::shared_ptr<const Data> data_;
    friend class Session;
};

/// Owner-side runtime work retained independently of a ParamOps frontend.
/// Implementations own pending receipt/restoration data and hold no Store or
/// frontend reference. Session::tick drives them outside SQL and notifications.
class SessionRuntimeClient {
public:
    virtual ~SessionRuntimeClient() = default;
    virtual void pump(std::int64_t nowMs) = 0;
    [[nodiscard]] virtual bool releasable() const noexcept = 0;
    /// False while older FIFO delivery or unstable capture prevents a base
    /// from being described as current. Stable owned gestures use their lease.
    [[nodiscard]] virtual bool presentationReady(const AppliedSource&,std::uint64_t) const noexcept {return true;}
    /// Called after endpoint revocation and before device destruction. Clear
    /// SDK pointers and mark work Detached; do not invoke a device here.
    virtual void sessionEnded() noexcept = 0;
};
} // namespace adi::engine
