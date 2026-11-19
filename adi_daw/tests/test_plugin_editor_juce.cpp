// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/vst3_host.hpp"
#include "juce/editor_host_component.hpp"
#include <cstdio>

namespace {
using namespace adi::device;
struct Counts{int created=0,closed=0,processorClosed=0;bool badOrder=false;};
class Processor final:public juce::AudioPluginInstance {
public:
    explicit Processor(Counts& counts):counts_(counts){}
    ~Processor()override{counts_.badOrder=getActiveEditor()!=nullptr;++counts_.processorClosed;}
    const juce::String getName()const override{return "Editor fixture";}
    void fillInPluginDescription(juce::PluginDescription& description)const override {description.name=getName();description.pluginFormatName="VST3";}
    void prepareToPlay(double,int)override{}
    void releaseResources()override{}
    void processBlock(juce::AudioBuffer<float>&,juce::MidiBuffer&)override{}
    double getTailLengthSeconds()const override{return 0;}
    bool acceptsMidi()const override{return false;}
    bool producesMidi()const override{return false;}
    int getNumPrograms()override{return 1;}
    int getCurrentProgram()override{return 0;}
    void setCurrentProgram(int)override{}
    const juce::String getProgramName(int)override{return {};}
    void changeProgramName(int,const juce::String&)override{}
    void getStateInformation(juce::MemoryBlock&)override{}
    void setStateInformation(const void*,int)override{}
    bool hasEditor()const override{return enabled;}
    class Editor final:public juce::AudioProcessorEditor {
    public:
        Editor(Processor& owner,Counts& counts):juce::AudioProcessorEditor(owner),counts_(counts) {
            ++counts_.created;setResizable(true,false);setResizeLimits(160,90,900,700);setSize(640,360);
        }
        ~Editor()override{++counts_.closed;}
    private:Counts& counts_;
    };
    juce::AudioProcessorEditor* createEditor()override{return refuse?nullptr:new Editor(*this,counts_);}
    bool enabled=true,refuse=false;
private:Counts& counts_;
};
}

bool runPluginEditorJuceTests() {
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* message){++checks;if(!ok){++failures;std::printf("FAIL editor JUCE: %s\n",message);}};
    Counts counts;
    auto processor=std::make_unique<Processor>(counts);auto* raw=processor.get();
    auto device=std::make_unique<Vst3Device>(std::move(processor),DeviceIdentity{});
    std::string error;
    raw->enabled=false;
    check(!device->createEditor(EditorMode::Hosted,error) && !error.empty(),"headless VST3 refuses cleanly");
    raw->enabled=true;raw->refuse=true;
    check(!device->createEditor(EditorMode::Hosted,error) && !error.empty(),"plugin refusal has a reason");
    raw->refuse=false;
    check(!device->createEditor(EditorMode::Floating,error),"VST3 uses hosted component mode");
    auto component=createEditorComponent(*device,error);
    check(component && error.empty() && counts.created==1,"factory creates one JUCE editor");
    if(!component)return false;
    check(!createEditorComponent(*device,error) && component->editor()->valid(),"duplicate floating content cannot steal or close existing editor");
    auto editor=component->editor();
    check(raw->getActiveEditor()==editor->component(),"JUCE active editor is registered");
    check(component->getWidth()==640 && component->getHeight()==360,"floating content starts at plugin's own size");
    check(editor->component()->getParentComponent()==component.get(),"same editor component is placed in floating content");
    check(device->createEditor(EditorMode::Hosted,error)==editor && counts.created==1,"second open shares current editor lease");
    editor->component()->setSize(700,400);
    check(component->getWidth()==700 && component->getHeight()==400,"plugin initiated resize updates wrapper immediately");
    component->setSize(1100,800);
    check(component->getWidth()==900 && component->getHeight()==700,"host resize obeys plugin constrainer");
    check(editor->hide() && !editor->visible() && editor->show() && editor->visible(),"visibility maps to JUCE component");
    component.reset();
    check(!editor->valid() && raw->getActiveEditor()==nullptr && counts.closed==1,"wrapper close unregisters active editor before deleting it");
    component=createEditorComponent(*device,error);
    check(component && counts.created==2,"editor reopens on same processor");
    editor=component->editor();device.reset();component->frame();
    check(!editor->valid() && !component->isVisible() && counts.closed==2 && counts.processorClosed==1 && !counts.badOrder,
          "device removal closes editor before processor and leaves safe wrapper/lease");
    component.reset();editor->close();check(counts.closed==2,"retained wrapper destruction never double deletes editor");
    std::printf("JUCE editor: %d checks, %d failures\n",checks,failures);
    return failures==0;
}
