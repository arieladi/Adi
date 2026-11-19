// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/cue_settings.hpp"
#include "adi/engine/host.hpp"
#include "adi/engine/mixer.hpp"
#include <memory>
namespace adi::engine {
class CueControlBatch {
public:
    void apply() const noexcept;
    void captureLanes(StripLaneBindings&) const;
private:
    struct Entry { void* target; void (*apply)(void*,const StripControlImage&) noexcept; StripControlImage image; const StripNode* node; };
    std::vector<Entry> entries_;
    friend class CueMonitor;
};
class CueMonitor {
public:
    CueMonitor();
    ~CueMonitor();
    CueMonitor(const CueMonitor&)=delete;
    CueMonitor& operator=(const CueMonitor&)=delete;
    /// Message thread, before realisation. Persistent nodes reuse rendered
    /// Post FX audio; the reference keeps its fader/pan/automation, Cue is PFL.
    void configure(const rows::Model&,const CueSettings&,const Transport*,const std::shared_ptr<StripAutomation>&,bool enabled);
    // Owner, after configure and before publication: bind Reference's private
    // gain stages to the same immutable effective-tempo generation as Main.
    void captureLanes(StripLaneBindings&) const;
    std::vector<MonitorPoint> points(const TapFn&) const;
    /// Publish only after the matching graph was prepared successfully.
    bool publish(const CueSettings&,double rate,int maxFrames,int channels,bool enabled,const Graph* prepared=nullptr) noexcept;
    [[nodiscard]] std::shared_ptr<const CueControlBatch> prepareControls(const rows::Model&,const CueSettings&,
        const std::shared_ptr<StripAutomation>&,bool enabled) const;
    void collect();
    struct Click {
        void* context=nullptr;
        void (*render)(void*,const AudioIo&) noexcept=nullptr;
    };
    /// Audio thread, after GraphHost::process. One Cue bus, with a five-ms
    /// complementary Main/reference swap; volume applies to cue and reference.
    void process(const GraphHost&,const AudioIo&,Click) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
