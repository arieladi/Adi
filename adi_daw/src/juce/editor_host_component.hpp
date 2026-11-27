// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "juce/device_model.hpp"
#include <juce_gui_basics/juce_gui_basics.h>

namespace adi::device {

/// Content for FloatingHost::openView. Owns the editor lease, never the device.
/// Call frame() on that window's existing clock (no independent timer). Closes
/// safely if the device disappears, and reports the plug-in's own client size.
class PluginEditorComponent final:public juce::Component,
                                   private juce::ComponentListener {
public:
    ~PluginEditorComponent()override;
    void frame();
    void resized()override;
    void parentHierarchyChanged()override;
    void visibilityChanged()override;
    [[nodiscard]] const std::shared_ptr<DeviceEditor>& editor()const noexcept{return editor_;}
    [[nodiscard]] const std::string& error()const noexcept{return error_;}
private:
    friend std::unique_ptr<PluginEditorComponent> createEditorComponent(DeviceInstance&,std::string&);
    explicit PluginEditorComponent(std::shared_ptr<DeviceEditor>);
    void attachNative();
    void componentMovedOrResized(juce::Component&,bool,bool resized)override;
    std::shared_ptr<DeviceEditor> editor_;
    // The native window a non-component editor (CLAP's, a native view) is parented to. OPAQUE and
    // painted (SC-322): a non-opaque desktop child with no title bar is a per-pixel layered window on
    // Windows, which shows what lies beneath it and in which the plug-in's own child window never draws
    // -- the CLAP window was a see-through frame.
    struct NativeParent final : juce::Component {
        NativeParent() { setOpaque(true); }
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colours::black); }
    } nativeParent_;
public:
    [[nodiscard]] const juce::Component& nativeParent() const noexcept { return nativeParent_; } // for its test
private:
    juce::Component::SafePointer<juce::Component> content_;
    void* outerHandle_=nullptr;
    std::string error_;
    bool adjusting_=false;
    double nativeScale_=1.0;
};

[[nodiscard]] std::unique_ptr<PluginEditorComponent> createEditorComponent(DeviceInstance&,std::string& error);

} // namespace adi::device
