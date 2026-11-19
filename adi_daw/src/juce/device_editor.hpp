// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace juce { class Component; }

namespace adi::device {

/// All editor operations, including last handle destruction, are message-thread
/// only. Never call from the audio callback. No project or journal state lives here.
enum class EditorMode { Floating, Hosted };
struct EditorSize { int width=0, height=0; };
struct EditorResizeHints {
    bool horizontal=true,vertical=true,preserveAspect=false;
    std::uint32_t aspectWidth=0,aspectHeight=0;
};
struct EditorParent {
    std::string api; // "win32", "cocoa" or "x11"; no framework-specific handle type
    std::uintptr_t handle=0;
};

/// One editor per loaded device. A device closes it BEFORE unloading its plug-in;
/// a retained handle then stays safely closed. Closing is idempotent. Hosts must
/// not keep a raw component pointer after close; use the owning bridge instead.
class DeviceEditor {
public:
    virtual ~DeviceEditor() = default;
    /// Reserve the single owning FloatingHost content. Sharing a lease for
    /// inspection must never create two wrappers that steal/destroy its view.
    [[nodiscard]] bool claimHostContent() noexcept {
        if(hostContentClaimed_)return false;
        hostContentClaimed_=true;return true;
    }
    [[nodiscard]] virtual EditorMode mode() const noexcept = 0;
    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual bool visible() const noexcept = 0;
    [[nodiscard]] virtual EditorSize size() const noexcept = 0;
    [[nodiscard]] virtual bool resizable() const noexcept = 0;
    [[nodiscard]] virtual EditorResizeHints resizeHints() const noexcept { return {}; }
    virtual bool attach(const EditorParent&) = 0; // parent (Hosted) or transient owner (Floating)
    virtual bool show() = 0;
    virtual bool hide() = 0;
    virtual bool resize(EditorSize&) = 0; // in/out: plug-in-adjusted size; Hosted only
    virtual bool setScale(double) = 0;
    virtual void setTitle(const std::string&) {}
    virtual void poll() {} // drain deferred protocol notifications, on the window clock
    virtual void close() = 0;
    [[nodiscard]] virtual juce::Component* component() noexcept { return nullptr; }
private:
    bool hostContentClaimed_=false;
};

} // namespace adi::device
