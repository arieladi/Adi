// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/clap_host.hpp"
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

namespace {
using namespace adi::device;
int checks=0,failures=0;
void check(bool ok,const char* message){++checks;if(!ok){++failures;std::printf("FAIL %s\n",message);}}
struct Fake {
    clap_plugin_t plugin{};
    const clap_host_t* host=nullptr;
    bool hasGui=true,allowFloating=true,allowHosted=true,allowCreate=true,allowSize=true,allowShow=true;
    bool allowHide=true,allowAttach=true,allowResize=true,allowAdjust=true,canResize=true,dead=false;
    bool floating=false,shown=false;
    std::uint32_t width=640,height=360;
    int creates=0,destroys=0,shows=0,hides=0,parents=0,transients=0,sets=0,scales=0,hints=0,afterDestroy=0;
    std::string api,title,order;
    Fake() {
        plugin.plugin_data=this;
        plugin.get_extension=[](const clap_plugin_t* p,const char* name)->const void* {
            return std::strcmp(name,CLAP_EXT_GUI)==0 && self(p).hasGui?&gui:nullptr;
        };
        plugin.destroy=[](const clap_plugin_t* p){auto& f=self(p);f.order+='p';f.dead=true;};
    }
    static Fake& self(const clap_plugin_t* p){auto& f=*static_cast<Fake*>(p->plugin_data);if(f.dead)++f.afterDestroy;return f;}
    static const clap_plugin_gui_t gui;
    std::unique_ptr<ClapDevice> device() {
        auto glue=std::make_unique<ClapHostGlue>();host=glue->host();glue->registerPlugin(&plugin);
        auto result=std::make_unique<ClapDevice>(&plugin,DeviceIdentity{});result->adoptGlue(std::move(glue));return result;
    }
    const clap_host_gui_t* hostGui()const{return static_cast<const clap_host_gui_t*>(host->get_extension(host,CLAP_EXT_GUI));}
};
const clap_plugin_gui_t Fake::gui={
        [](const clap_plugin_t* p,const char*,bool floating){auto& f=self(p);return floating?f.allowFloating:f.allowHosted;},
        nullptr,
        [](const clap_plugin_t* p,const char* api,bool floating){auto& f=self(p);++f.creates;f.floating=floating;f.api=api?api:"";return f.allowCreate;},
        [](const clap_plugin_t* p){auto& f=self(p);++f.destroys;f.order+='g';f.shown=false;},
        [](const clap_plugin_t* p,double){++self(p).scales;return true;},
        [](const clap_plugin_t* p,std::uint32_t* width,std::uint32_t* height){auto& f=self(p);*width=f.width;*height=f.height;return f.allowSize;},
        [](const clap_plugin_t* p){return self(p).canResize;},
        [](const clap_plugin_t* p,clap_gui_resize_hints_t* hints){++self(p).hints;*hints={true,false,true,16,9};return true;},
        [](const clap_plugin_t* p,std::uint32_t* width,std::uint32_t* height){*width=(*width/10)*10;*height=(*height/10)*10;return self(p).allowAdjust;},
        [](const clap_plugin_t* p,std::uint32_t width,std::uint32_t height){auto& f=self(p);++f.sets;if(!f.allowResize)return false;f.width=width;f.height=height;return true;},
        [](const clap_plugin_t* p,const clap_window_t*){auto& f=self(p);++f.parents;return f.allowAttach;},
        [](const clap_plugin_t* p,const clap_window_t*){auto& f=self(p);++f.transients;return f.allowAttach;},
        [](const clap_plugin_t* p,const char* title){self(p).title=title;},
        [](const clap_plugin_t* p){auto& f=self(p);++f.shows;if(!f.allowShow)return false;f.shown=true;return true;},
        [](const clap_plugin_t* p){auto& f=self(p);++f.hides;if(!f.allowHide)return false;f.shown=false;return true;}
    };

EditorParent parent() {
#if defined(_WIN32)
    return {"win32",123};
#elif defined(__APPLE__)
    return {"cocoa",123};
#else
    return {"x11",123};
#endif
}
void refused() {
    std::string error;
    MissingDevice missing(DeviceIdentity{});
    check(!missing.createEditor(EditorMode::Hosted,error) && !error.empty(),"missing device reports no editor");
    Fake f;auto device=f.device();
    check(!device->createEditor(static_cast<EditorMode>(99),error),"unknown editor mode refused");
    f.hasGui=false;
    check(!device->createEditor(EditorMode::Hosted,error) && !error.empty(),"absent extension refuses with a reason");
    f.hasGui=true;f.allowHosted=false;f.allowFloating=false;
    check(!device->createEditor(EditorMode::Hosted,error) && f.creates==0,"unsupported API/mode never calls create");
    f.allowHosted=true;f.allowCreate=false;
    check(!device->createEditor(EditorMode::Hosted,error) && f.creates==1 && f.destroys==0,"failed create is not destroyed as a live GUI");
    f.allowCreate=true;f.allowSize=false;
    check(!device->createEditor(EditorMode::Hosted,error) && f.destroys==1,"missing size rolls back a created hosted GUI");
    f.allowSize=true;f.width=0;
    check(!device->createEditor(EditorMode::Hosted,error) && f.destroys==2,"zero editor dimension refused and cleaned up");
    f.width=std::numeric_limits<std::uint32_t>::max();
    check(!device->createEditor(EditorMode::Hosted,error) && f.destroys==3,"dimension exceeding component integer range refused");
    check(!f.hostGui()->request_show(f.host),"failed construction leaves no active notification mailbox");
}
void floating() {
    Fake f;auto device=f.device();std::string error="stale";
    auto editor=device->createEditor(EditorMode::Floating,error);
    check(editor && editor->valid() && error.empty() && f.floating,"native floating mode reaches CLAP create(true)");if(!editor)return;
    check(editor->size().width==640 && editor->size().height==360 && !editor->resizable() && !editor->component(),"native floating size is owned/readable and has no duplicate component");
    check(device->createEditor(EditorMode::Floating,error)==editor && f.creates==1,"second open shares one editor");
    check(!device->createEditor(EditorMode::Hosted,error) && !error.empty(),"changing mode requires closing current editor");
    editor->setTitle("Synth ✦");check(f.title=="Synth ✦","native window gets its title");
    check(editor->attach(parent()) && f.transients==1 && f.parents==0,"floating attaches transient owner, never embedded parent");
    check(editor->show() && editor->show() && editor->visible() && f.shows==1,"show is idempotent and visibility follows successful call");
    f.allowHide=false;check(!editor->hide() && editor->visible(),"failed hide does not pretend window is hidden");
    f.allowHide=true;check(editor->hide() && !editor->visible() && editor->show(),"hidden editor can be shown again");
    EditorSize size{800,450};check(!editor->resize(size) && f.sets==0,"floating resize never calls embedded-only methods");
    check(!f.hostGui()->request_resize(f.host,800,450),"embedded resize request refused for native floating window");
    const int hides=f.hides;
    f.hostGui()->closed(f.host,true);device->pumpMainThread();
    check(!editor->valid() && !editor->visible() && f.destroys==1 && f.hides==hides,"closed(true) acknowledges destroy without hiding a destroyed window");
    check(!editor->show() && !editor->attach(parent()),"closed handle never calls a plugin GUI again");
    auto next=device->createEditor(EditorMode::Floating,error);
    check(next && next!=editor && f.creates==2,"closed GUI can be recreated as a new lease");
    check(!next->visible(),"old closed notification does not close the new GUI");
    f.width=800;next->poll();check(next->size().width==800,"native floating size refreshes on owner clock");
    device.reset();
    check(!next->valid() && f.order=="ggp" && f.afterDestroy==0,"device closes all live GUI resources before plugin destroy");
    next->close();editor->close();check(f.destroys==2 && f.afterDestroy==0,"retained leases are safe after device destruction");
}
void hosted() {
    Fake f;auto device=f.device();std::string error;
    auto editor=device->createEditor(EditorMode::Hosted,error);
    check(editor && !f.floating && editor->resizable(),"hosted mode creates one embeddable GUI");if(!editor)return;
    check(!editor->show() && f.shows==0,"hosted GUI cannot show before successful parent attachment");
    check(!editor->attach({"unknown",42}) && !editor->attach({parent().api,0}),"invalid API and null native handle refused");
    f.allowAttach=false;check(!editor->attach(parent()) && !editor->show(),"failed parent does not mark GUI attached");
    f.allowAttach=true;check(editor->attach(parent()) && f.parents==2 && f.transients==0 && editor->show(),"hosted attaches real parent and then shows");
    EditorSize requested{813,467};
    check(editor->resize(requested) && requested.width==810 && requested.height==460 && editor->size().width==810,"host drag applies plug-in adjusted size");
    const auto before=editor->size();f.allowResize=false;requested={900,500};
    check(!editor->resize(requested) && editor->size().width==before.width,"failed resize leaves last accepted size");
    f.allowResize=true;requested={-1,10};check(!editor->resize(requested),"negative resize refused");
    check(!editor->setScale(0) && !editor->setScale(std::numeric_limits<double>::infinity()),"invalid scales never reach native API");
#if defined(__APPLE__)
    check(!editor->setScale(2) && f.scales==0,"Cocoa logical pixels never call set_scale");
#else
    check(editor->setScale(2) && f.scales==1,"physical-pixel API forwards explicit scale");
#endif
    const auto hints=editor->resizeHints();
    check(hints.horizontal && !hints.vertical && hints.preserveAspect && hints.aspectWidth==16 && hints.aspectHeight==9,"resize hints expose axis and aspect constraints");
    const auto* host=f.hostGui();bool accepted=false;
    const auto shows=f.shows,hides=f.hides,sets=f.sets,hintReads=f.hints;
    std::thread producer([&]{accepted=host->request_resize(f.host,1000,600);host->resize_hints_changed(f.host);(void)host->request_hide(f.host);});producer.join();
    check(accepted && f.shows==shows && f.hides==hides && f.sets==sets && f.hints==hintReads,"worker GUI requests only publish mailboxes without calling plug-in");
    device->pumpMainThread();
    check(editor->size().width==1000 && editor->size().height==600 && !editor->visible() && f.hides==hides+1 && f.sets==sets && f.hints==hintReads+1,
          "owner pump accepts plugin resize without echoing set_size and drains visibility/hints");
    check(!host->request_resize(f.host,0,10) && !host->request_resize(f.host,UINT_MAX,10),"malformed plugin resize requests refused");
    check(host->request_show(f.host),"plugin can ask to show an existing editor");editor->poll();
    check(editor->visible() && f.shows==shows+1,"window clock can also drain notifications");
    host->closed(f.host,false);editor->poll();
    check(!editor->valid() && f.destroys==1,"closed(false) destroys surviving GUI resources exactly once");
    check(!host->request_hide(f.host),"notifications refused once editor has closed");
    auto next=device->createEditor(EditorMode::Hosted,error);check(next && next->valid(),"hosted GUI reopens after plugin close");
    next.reset();check(f.destroys==2,"last editor lease closes resources even while device lives");
    f.allowSize=false;auto native=device->createEditor(EditorMode::Floating,error);
    check(native && native->size().width==0 && native->show(),"native floating GUI can own geometry without reporting an embedded size");
}
} // namespace
int main(){refused();floating();hosted();std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
