// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mixer.hpp"
#include "ui_shell.hpp"
#include <algorithm>
#include <cmath>
namespace adi::ui {
MixerStrip::MixerStrip(MixerPanel &panel) : panel_(panel) {
    for (auto *s : {&volume, &pan}) {
        addAndMakeVisible(*s);
        s->setSliderStyle(juce::Slider::LinearHorizontal);
        s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 54, 20);
        s->setScrollWheelEnabled(false);
        s->onDragStart = [this] { dragging_ = true; };
    }
    volume.setRange(-90, 6, .01);
    volume.setTextValueSuffix(" dB");
    pan.setRange(-1, 1, .01);
    pan.setDoubleClickReturnValue(true, 0);
    volume.onDragEnd = [this] {
        dragging_ = false;
        panel_.root.submit("mixer.setVolume", {{"id", track_}, {"db", volume.getValue()}});
    };
    pan.onDragEnd = [this] {
        dragging_ = false;
        panel_.root.submit("mixer.setPan", {{"id", track_}, {"pan", pan.getValue()}});
    };
    // Keyboard/text edits are gestures too; a drag commits only on release.
    volume.onValueChange = [this] {
        if (!dragging_)
            panel_.root.submit("mixer.setVolume", {{"id", track_}, {"db", volume.getValue()}});
    };
    pan.onValueChange = [this] {
        if (!dragging_)
            panel_.root.submit("mixer.setPan", {{"id", track_}, {"pan", pan.getValue()}});
    };
    for (auto *b : {&active, &solo, &learn})
        addAndMakeVisible(*b);
    active.onClick = [this] {
        if (auto *t = panel_.root.reader().findTrack(track_))
            panel_.root.submit("track.setMute", {{"id", track_}, {"muted", !t->muted}});
    };
    solo.onClick = [this] {
        if (auto *t = panel_.root.reader().findTrack(track_))
            panel_.root.submit("track.setSolo", {{"id", track_}, {"soloed", !t->soloed}});
    };
    learn.onClick = [this] { panel_.learnMenu(track_, learn); };
}
void MixerStrip::bind(std::int64_t id) {
    track_ = id;
    frame(true);
}
void MixerStrip::frame(bool changed) {
    const auto *t = panel_.root.reader().findTrack(track_);
    setEnabled(t != nullptr);
    if (changed && t && !dragging_) {
        volume.setValue(t->volumeDb, juce::dontSendNotification);
        pan.setValue(t->pan, juce::dontSendNotification);
        active.setToggleState(!t->muted, juce::dontSendNotification);
        solo.setToggleState(t->soloed, juce::dontSendNotification);
        repaint();
    }
    const auto *m = panel_.meter ? panel_.meter(track_) : nullptr;
    const float p = m ? m->peak.load(std::memory_order_relaxed) : 0;
    const float r = m ? m->rms.load(std::memory_order_relaxed) : 0;
    if (p != peak_ || r != rms_) {
        peak_ = p;
        rms_ = r;
        repaint(getWidth() - 12, 24, 10, 94);
    }
}
void MixerStrip::resized() {
    volume.setBounds(4, 27, getWidth() - 22, 24);
    pan.setBounds(4, 55, getWidth() - 22, 24);
    active.setBounds(4, 86, 50, 24);
    solo.setBounds(58, 86, 50, 24);
    learn.setBounds(112, 86, std::max(30, getWidth() - 132), 24);
}
void MixerStrip::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff242b35));
    g.setColour(juce::Colour(0xffdee5ed));
    g.setFont(juce::FontOptions(13.f));
    if (auto *t = panel_.root.reader().findTrack(track_))
        g.drawText(t->name, 6, 2, getWidth() - 12, 22, juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff11161c));
    g.fillRect(getWidth() - 12, 24, 8, 86);
    auto height = [](float v) {
        return static_cast<int>(
            86 * std::clamp((20 * std::log10(std::max(v, 1.e-6f)) + 60) / 60, 0.f, 1.f));
    };
    const auto h = height(rms_);
    g.setColour(juce::Colour(0xff65cf91));
    g.fillRect(getWidth() - 12, 110 - h, 8, h);
    g.setColour(peak_ >= 1 ? juce::Colours::red : juce::Colours::white);
    g.fillRect(getWidth() - 12, 110 - height(peak_), 8, 1);
}
MixerPanel::MixerPanel(AdiRootComponent &r) : root(r) {
    addAndMakeVisible(gain);
    gain.onClick = [this] {
        juce::PopupMenu menu;
        menu.addItem(1, "Gated RMS: -18 dBFS");
        menu.addItem(2, "Integrated loudness: -18 LUFS");
        auto safe = juce::Component::SafePointer<MixerPanel>(this);
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&gain), [safe](int id) {
            if (safe && id && safe->autoGain)
                safe->autoGain(id == 2);
        });
    };
}
void MixerPanel::resized() {
    gain.setBounds(6,5,std::max(0,getWidth()-12),26);
    const auto capacity=static_cast<std::size_t>(std::max(0,getHeight()-38)/rowHeight+1);
    while(strips_.size()<capacity){auto strip=std::make_unique<MixerStrip>(*this);addChildComponent(*strip);strips_.push_back(std::move(strip));}
    // Retain a dragging strip even if the window shrinks around it.
    while(strips_.size()>capacity&&!strips_.back()->dragging())strips_.pop_back();
    for(std::size_t i=0;i<strips_.size();++i)strips_[i]->setBounds(4,38+static_cast<int>(i)*rowHeight,getWidth()-8,rowHeight-4);
    frame(true);
}
void MixerPanel::frame(bool changed) {
    const auto& tracks=root.reader().tracks();
    const auto wanted=std::min(tracks.size(),strips_.size());
    bool dragging=false;for(const auto& s:strips_)dragging|=s->dragging();
    if(!dragging){
        first_=std::min(first_,tracks.size()>wanted?tracks.size()-wanted:0);
        for(std::size_t i=0;i<strips_.size();++i){auto& s=*strips_[i];s.setVisible(i<wanted);if(i<wanted&&s.track()!=tracks[first_+i]->id)s.bind(tracks[first_+i]->id);}
    }
    for(auto& s:strips_)if(s->isVisible())s->frame(changed);
}
void MixerPanel::paint(juce::Graphics &g) { g.fillAll(juce::Colour(0xff21262e)); }
void MixerPanel::scroll(int rows) {
    for (auto &s : strips_)
        if (s->dragging())
            return;
    first_ = static_cast<std::size_t>(std::max(0, static_cast<int>(first_) + rows));
    frame(true);
}
void MixerPanel::mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &w) {
    scroll(w.deltaY > 0 ? -1 : 1);
}
void MixerPanel::learnMenu(std::int64_t track, juce::Component &anchor) {
    juce::PopupMenu menu;
    const bool v = shadowed && shadowed(track, "volume"), p = shadowed && shadowed(track, "pan");
    menu.addItem(1, v ? "Learn volume (existing binding shadowed)" : "Learn volume");
    menu.addItem(2, p ? "Learn pan (existing binding shadowed)" : "Learn pan");
    menu.addItem(3, "Unbind volume");
    menu.addItem(4, "Unbind pan");
    auto safe = juce::Component::SafePointer<MixerPanel>(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options{}.withTargetComponent(&anchor), [safe, track](int id) {
            if (safe && id && safe->midiLearn)
                safe->midiLearn(track, (id == 1 || id == 3) ? "volume" : "pan", id >= 3);
        });
}
} // namespace adi::ui
