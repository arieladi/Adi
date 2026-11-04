// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/clap_host.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace adi::device {
namespace {
const char* nativeApi() noexcept {
#if defined(_WIN32)
    return CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#else
    return CLAP_WINDOW_API_X11;
#endif
}
bool validSize(std::uint32_t width,std::uint32_t height) noexcept {
    const auto bound=static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    return width>0 && height>0 && width<=bound && height<=bound;
}
class ClapEditor final:public DeviceEditor {
public:
    ClapEditor(const clap_plugin_t* plugin,const clap_plugin_gui_t* gui,ClapHostGlue* glue,
               EditorMode mode,const char* api):plugin_(plugin),gui_(gui),glue_(glue),mode_(mode),api_(api?api:"") {}
    ~ClapEditor() override { close(); }
    bool initialize(std::string& error) {
        if(!gui_->create(plugin_,api_.empty()?nullptr:api_.c_str(),mode_==EditorMode::Floating)) {
            error="CLAP refused to create its editor";plugin_=nullptr;return false;
        }
        created_=true;
        if(glue_)glue_->enableGuiRequests(true,mode_==EditorMode::Hosted);
        std::uint32_t width=0,height=0;
        const bool sized=gui_->get_size && gui_->get_size(plugin_,&width,&height) && validSize(width,height);
        if(!sized && mode_==EditorMode::Hosted) {
            error="CLAP editor did not supply a valid size";close();return false;
        }
        // Native floating windows own their geometry and may not expose it.
        if(sized)size_={static_cast<int>(width),static_cast<int>(height)};
        resizable_=mode_==EditorMode::Hosted && gui_->can_resize && gui_->can_resize(plugin_);
        readHints();return true;
    }
    EditorMode mode()const noexcept override{return mode_;}
    bool valid()const noexcept override{return created_;}
    bool visible()const noexcept override{return visible_;}
    EditorSize size()const noexcept override{return size_;}
    bool resizable()const noexcept override{return created_ && resizable_;}
    EditorResizeHints resizeHints()const noexcept override{return hints_;}
    bool attach(const EditorParent& parent) override {
        if(!created_ || parent.handle==0 || parent.api!=nativeApi())return false;
        clap_window_t window{};window.api=nativeApi();
        if(parent.api==CLAP_WINDOW_API_X11)window.x11=static_cast<clap_xwnd>(parent.handle);
        else window.ptr=reinterpret_cast<void*>(parent.handle);
        if(mode_==EditorMode::Floating)return gui_->set_transient && gui_->set_transient(plugin_,&window);
        attached_=gui_->set_parent && gui_->set_parent(plugin_,&window);return attached_;
    }
    bool show()override {
        if(!created_ || (mode_==EditorMode::Hosted && !attached_))return false;
        if(visible_)return true;
        if(!gui_->show(plugin_))return false;
        visible_=true;return true;
    }
    bool hide()override {
        if(!created_)return false;
        if(!visible_)return true;
        if(!gui_->hide(plugin_))return false;
        visible_=false;return true;
    }
    bool resize(EditorSize& requested)override {
        if(!resizable() || requested.width<=0 || requested.height<=0 || !gui_->adjust_size || !gui_->set_size)return false;
        auto width=static_cast<std::uint32_t>(requested.width),height=static_cast<std::uint32_t>(requested.height);
        if(!gui_->adjust_size(plugin_,&width,&height) || !validSize(width,height) || !gui_->set_size(plugin_,width,height))return false;
        size_={static_cast<int>(width),static_cast<int>(height)};requested=size_;return true;
    }
    bool setScale(double scale)override {
        // Cocoa's dimensions are logical; gui.h explicitly forbids set_scale.
        if(!created_ || api_==CLAP_WINDOW_API_COCOA || !std::isfinite(scale) || scale<=0 ||
           !gui_->set_scale || !gui_->set_scale(plugin_,scale))return false;
        std::uint32_t width=0,height=0;
        if(gui_->get_size && gui_->get_size(plugin_,&width,&height) && validSize(width,height))
            size_={static_cast<int>(width),static_cast<int>(height)};
        return true;
    }
    void setTitle(const std::string& title)override {
        if(created_ && mode_==EditorMode::Floating && gui_->suggest_title)gui_->suggest_title(plugin_,title.c_str());
    }
    void poll()override {
        if(!created_ || !glue_)return;
        const auto requests=glue_->takeGuiRequests();
        if(requests.closed){visible_=false;close();return;}
        if(requests.hints)readHints();
        if(requests.size && mode_==EditorMode::Hosted) {
            size_={static_cast<int>(requests.size>>32),static_cast<int>(requests.size&0xffffffffULL)};
        }
        if(requests.visibility==1)(void)show();
        else if(requests.visibility==2)(void)hide();
        if(mode_==EditorMode::Floating && gui_->get_size) {
            std::uint32_t width=0,height=0;
            if(gui_->get_size(plugin_,&width,&height) && validSize(width,height))
                size_={static_cast<int>(width),static_cast<int>(height)};
        }
    }
    void close()override {
        if(!created_)return;
        if(glue_)glue_->enableGuiRequests(false);
        // gui.closed(was_destroyed=true) still requires destroy to acknowledge.
        // Do not call hide on a window whose connection was lost.
        if(visible_) (void)gui_->hide(plugin_);
        visible_=false;created_=false;
        gui_->destroy(plugin_);plugin_=nullptr;gui_=nullptr;glue_=nullptr;
    }
private:
    void readHints() {
        if(mode_!=EditorMode::Hosted)return;
        resizable_=gui_->can_resize && gui_->can_resize(plugin_);
        clap_gui_resize_hints_t hints{};
        if(gui_->get_resize_hints && gui_->get_resize_hints(plugin_,&hints))
            hints_={hints.can_resize_horizontally,hints.can_resize_vertically,hints.preserve_aspect_ratio,
                    hints.aspect_ratio_width,hints.aspect_ratio_height};
    }
    EditorResizeHints hints_;
    const clap_plugin_t* plugin_;
    const clap_plugin_gui_t* gui_;
    ClapHostGlue* glue_;
    EditorMode mode_;
    std::string api_;
    EditorSize size_;
    bool created_=false,visible_=false,resizable_=false,attached_=false;
};
} // namespace

std::shared_ptr<DeviceEditor> ClapDevice::createEditor(EditorMode mode,std::string& error) {
    error.clear();
    if(mode!=EditorMode::Hosted && mode!=EditorMode::Floating){error="Unknown editor mode";return {};}
    if(auto existing=editor_.lock();existing && existing->valid()) {
        if(existing->mode()==mode)return existing;
        error="Close the current editor before changing its mode";return {};
    }
    const auto* gui=plugin_ && plugin_->get_extension
        ?static_cast<const clap_plugin_gui_t*>(plugin_->get_extension(plugin_,CLAP_EXT_GUI)):nullptr;
    if(!gui || !gui->is_api_supported || !gui->create || !gui->destroy || !gui->show || !gui->hide ||
       (mode==EditorMode::Hosted && (!gui->get_size || !gui->set_parent))) {
        error="This CLAP has no usable GUI extension";return {};
    }
    const char* api=nativeApi();const bool floating=mode==EditorMode::Floating;
    if(!gui->is_api_supported(plugin_,api,floating)) {
        if(!floating || !gui->is_api_supported(plugin_,nullptr,true)) {
            error="CLAP editor does not support this window API and mode";return {};
        }
        api=nullptr;
    }
    auto editor=std::make_shared<ClapEditor>(plugin_,gui,glue_.get(),mode,api);
    if(!editor->initialize(error))return {};
    editor_=editor;return editor;
}

ClapHostGlue::GuiRequests ClapHostGlue::takeGuiRequests() noexcept {
    return {guiSize_.exchange(0,std::memory_order_acq_rel),guiVisibility_.exchange(0,std::memory_order_acq_rel),
            guiClosed_.exchange(false,std::memory_order_acq_rel),guiHints_.exchange(false,std::memory_order_acq_rel)};
}
void ClapHostGlue::enableGuiRequests(bool enabled,bool hosted) noexcept {
    guiEnabled_.store(false,std::memory_order_release);
    (void)takeGuiRequests();guiHosted_.store(hosted,std::memory_order_release);
    guiEnabled_.store(enabled,std::memory_order_release);
}
void ClapHostGlue::guiHints(const clap_host_t* host) {
    auto* self=static_cast<ClapHostGlue*>(host->host_data);
    if(self->guiEnabled_.load(std::memory_order_acquire))self->guiHints_.store(true,std::memory_order_release);
}
bool ClapHostGlue::guiResize(const clap_host_t* host,std::uint32_t width,std::uint32_t height) {
    auto* self=static_cast<ClapHostGlue*>(host->host_data);
    if(!self->guiEnabled_.load(std::memory_order_acquire) || !self->guiHosted_.load(std::memory_order_acquire) || !validSize(width,height))return false;
    self->guiSize_.store((static_cast<std::uint64_t>(width)<<32)|height,std::memory_order_release);return true;
}
bool ClapHostGlue::guiShow(const clap_host_t* host) {
    auto* self=static_cast<ClapHostGlue*>(host->host_data);
    if(!self->guiEnabled_.load(std::memory_order_acquire))return false;
    self->guiVisibility_.store(1,std::memory_order_release);return true;
}
bool ClapHostGlue::guiHide(const clap_host_t* host) {
    auto* self=static_cast<ClapHostGlue*>(host->host_data);
    if(!self->guiEnabled_.load(std::memory_order_acquire))return false;
    self->guiVisibility_.store(2,std::memory_order_release);return true;
}
void ClapHostGlue::guiClosed(const clap_host_t* host,bool) {
    auto* self=static_cast<ClapHostGlue*>(host->host_data);
    if(self->guiEnabled_.load(std::memory_order_acquire))self->guiClosed_.store(true,std::memory_order_release);
}
} // namespace adi::device
