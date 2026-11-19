// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/editor_host_component.hpp"

namespace adi::device {
PluginEditorComponent::PluginEditorComponent(std::shared_ptr<DeviceEditor> editor):editor_(std::move(editor)) {
    if(!editor_ || !editor_->valid())return;
    content_=editor_->component();
    if(content_) {addAndMakeVisible(content_.getComponent());content_->addComponentListener(this);}
    const auto size=editor_->size();setSize(size.width,size.height);
}
PluginEditorComponent::~PluginEditorComponent() {
    if(content_)content_->removeComponentListener(this);
    if(editor_)editor_->close(); // destroy native GUI before destroying its parent
    nativeParent_.removeFromDesktop();
}
void PluginEditorComponent::attachNative() {
    if(!editor_ || !editor_->valid() || content_)return;
    auto* peer=getPeer();
    if(!peer || !isShowing()) {
        (void)editor_->hide();nativeParent_.setVisible(false);return;
    }
    void* outer=peer->getNativeHandle();
#if JUCE_MAC
    const auto scale=1.0; // Cocoa native API dimensions are already logical
#else
    const auto scale=peer->getPlatformScaleFactor();
#endif
    if(scale>0 && scale!=nativeScale_) {
        nativeScale_=scale;(void)editor_->setScale(scale);
    }
    if(outer!=outerHandle_ || !nativeParent_.getPeer() || !error_.empty()) {
        (void)editor_->hide();nativeParent_.removeFromDesktop();outerHandle_=outer;
        nativeParent_.addToDesktop(0,outer);
        EditorParent parent;
#if JUCE_WINDOWS
        parent.api="win32";
#elif JUCE_MAC
        parent.api="cocoa";
#else
        parent.api="x11";
#endif
        auto* nativePeer=nativeParent_.getPeer();
        parent.handle=nativePeer?reinterpret_cast<std::uintptr_t>(nativePeer->getNativeHandle()):0;
        if(!editor_->attach(parent)){error_="Plug-in refused its floating host parent";return;}
        error_.clear();
    }
    const auto top=getTopLevelComponent();
    const auto position=top->getLocalPoint(this,juce::Point<int>{});
    nativeParent_.setBounds(position.x,position.y,getWidth(),getHeight());
    nativeParent_.setVisible(true);
    if(!editor_->show())error_="Plug-in refused to show its editor";
}
void PluginEditorComponent::frame() {
    if(!editor_)return;
    editor_->poll();
    if(!editor_->valid()){nativeParent_.setVisible(false);setVisible(false);return;}
    auto size=editor_->size();
    if(!content_) {size.width=juce::roundToInt(size.width/nativeScale_);size.height=juce::roundToInt(size.height/nativeScale_);}
    if(size.width!=getWidth() || size.height!=getHeight()) {
        adjusting_=true;setSize(size.width,size.height);adjusting_=false;
    }
    attachNative();
}
void PluginEditorComponent::resized() {
    if(!editor_ || !editor_->valid() || adjusting_)return;
    const auto scale=content_?1.0:nativeScale_;
    EditorSize requested{juce::roundToInt(getWidth()*scale),juce::roundToInt(getHeight()*scale)};
    if(editor_->resize(requested)) {
        adjusting_=true;setSize(juce::roundToInt(requested.width/scale),juce::roundToInt(requested.height/scale));adjusting_=false;
    } else {
        const auto size=editor_->size();
        adjusting_=true;setSize(juce::roundToInt(size.width/scale),juce::roundToInt(size.height/scale));adjusting_=false;
    }
    if(content_)content_->setTopLeftPosition(0,0);
    attachNative();
}
void PluginEditorComponent::parentHierarchyChanged(){attachNative();}
void PluginEditorComponent::visibilityChanged() {
    if(!editor_ || !editor_->valid())return;
    if(content_){if(isShowing())(void)editor_->show();else (void)editor_->hide();}
    else attachNative();
}
void PluginEditorComponent::componentMovedOrResized(juce::Component& component,bool,bool resized) {
    if(adjusting_ || !resized)return;
    adjusting_=true;setSize(component.getWidth(),component.getHeight());adjusting_=false;
}
std::unique_ptr<PluginEditorComponent> createEditorComponent(DeviceInstance& device,std::string& error) {
    auto editor=device.createEditor(EditorMode::Hosted,error);
    if(!editor)return {};
    if(!editor->claimHostContent()){error="This editor already has floating host content";return {};}
    return std::unique_ptr<PluginEditorComponent>(new PluginEditorComponent(std::move(editor)));
}
} // namespace adi::device
