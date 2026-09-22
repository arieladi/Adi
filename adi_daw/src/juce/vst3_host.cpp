// SPDX-License-Identifier: GPL-3.0-or-later
//
// See vst3_host.hpp, in particular for what JUCE's host path cannot carry.

#include "juce/vst3_host.hpp"

#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstphysicalui.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace adi::device {

namespace {

/// VST3 identifies a parameter by a 32-bit `ParamID`, and `plugin_params`
/// stores TEXT. The conversion is fixed-width hex so it sorts stably and can
/// never collide with a CLAP or Pd symbol, which are the other things that end
/// up in that column.
std::string paramIdToText(juce::AudioProcessorParameter* p) {
    if (auto* withId = dynamic_cast<juce::HostedAudioProcessorParameter*>(p))
        return withId->getParameterID().toStdString();
    // No hosted id: fall back to the index, tagged so it is obviously an
    // index rather than a plugin-supplied symbol. A project saved against
    // such a plugin is index-bound, which SPEC 6.3.3 warns about.
    char buf[24];
    std::snprintf(buf, sizeof buf, "idx:%d", p != nullptr ? p->getParameterIndex() : -1);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// Vst3Host
// ---------------------------------------------------------------------------

Vst3Host::Vst3Host() {
    // NOT addDefaultFormats(). That adds every format JUCE was compiled with,
    // which is how an AU host appears on a Mac without anyone deciding to add
    // one. ADR-0041 says VST3 and nothing else, and the one-line difference
    // between these two calls is the whole enforcement.
    formats_.addFormat(std::make_unique<juce::VST3PluginFormat>());
}

std::vector<std::string> Vst3Host::formatNames() const {
    std::vector<std::string> out;
    for (int i = 0; i < formats_.getNumFormats(); ++i)
        out.push_back(formats_.getFormat(i)->getName().toStdString());
    return out;
}

juce::FileSearchPath Vst3Host::defaultSearchPaths() const {
    juce::VST3PluginFormat fmt;
    return fmt.getDefaultLocationsToSearch();
}

void Vst3Host::scan(const juce::FileSearchPath& paths, juce::KnownPluginList& into) {
    for (int i = 0; i < formats_.getNumFormats(); ++i) {
        auto* fmt = formats_.getFormat(i);
        juce::PluginDirectoryScanner scanner(into, *fmt, paths,
                                             /*recursive*/ true,
                                             /*deadMansPedal*/ juce::File());
        juce::String name;
        while (scanner.scanNextFile(true, name)) { /* one plugin per turn */ }
    }
}

std::unique_ptr<juce::AudioPluginInstance> Vst3Host::instantiate(
    const juce::PluginDescription& desc, double sampleRate, int blockSize,
    std::string& error) {
    juce::String err;
    auto inst = formats_.createPluginInstance(desc, sampleRate, blockSize, err);
    if (inst == nullptr) error = err.toStdString();
    return inst;
}

DeviceIdentity identityOf(const juce::PluginDescription& desc) {
    DeviceIdentity id;
    // Lower-case, matching plugin_refs.format's admitted spellings.
    id.format  = desc.pluginFormatName.toLowerCase().toStdString();
    if (id.format.empty()) id.format = "vst3";
    id.uid     = desc.createIdentifierString().toStdString();
    id.name    = desc.name.toStdString();
    id.vendor  = desc.manufacturerName.toStdString();
    id.version = desc.version.toStdString();
    return id;
}

std::unique_ptr<DeviceInstance> Vst3Host::makeDevice(const juce::PluginDescription& desc,
                                                     double sampleRate, int blockSize,
                                                     std::string& error) {
    const DeviceIdentity id = identityOf(desc);
    auto inst = instantiate(desc, sampleRate, blockSize, error);
    if (inst == nullptr) {
        // ADR-0011 / SPEC 7.1. Not nullptr, not an exception, not a skip: the
        // device stays in the chain as a bypassed placeholder carrying the
        // identity, so the signal path is unchanged and the user is told what
        // is missing rather than that "a plugin" is.
        return std::make_unique<MissingDevice>(id);
    }
    return std::make_unique<Vst3Device>(std::move(inst), id);
}

// ---------------------------------------------------------------------------
// Vst3Device
// ---------------------------------------------------------------------------

namespace {

/// Everything `engine::resolveRoute` needs, asked of the plugin's EDIT
/// CONTROLLER on the message thread (ADR-0097).
///
/// JUCE keeps its controller private and publishes only `IComponent`, so the
/// controller is reachable here only when the component IS the controller --
/// a single-component plugin. For a plugin with a separate controller class,
/// which includes every JUCE-built plugin, this returns `controllerReachable
/// = false` and the route is the user's to choose.
///
/// Every interface obtained through queryInterface is released before
/// returning: each one carries a reference, and a probe that leaked them would
/// keep a plugin's controller alive after the plugin was deleted.
engine::ExpressionCaps probeExpressionCaps(juce::AudioPluginInstance& inst) {
    namespace SV = Steinberg::Vst;
    engine::ExpressionCaps caps;

    const auto* client = inst.getVST3Client();
    if (client == nullptr) return caps;
    auto* comp = client->getIComponentPtr();
    if (comp == nullptr) return caps;

    void* raw = nullptr;
    if (comp->queryInterface(SV::IEditController::iid, &raw) != Steinberg::kResultOk ||
        raw == nullptr)
        return caps;
    auto* ec = static_cast<SV::IEditController*>(raw);
    caps.controllerReachable = true;

    raw = nullptr;
    if (ec->queryInterface(SV::INoteExpressionController::iid, &raw) == Steinberg::kResultOk &&
        raw != nullptr) {
        auto* nec = static_cast<SV::INoteExpressionController*>(raw);
        const Steinberg::int32 n = nec->getNoteExpressionCount(0, 0);
        for (Steinberg::int32 i = 0; i < n; ++i) {
            SV::NoteExpressionTypeInfo info{};
            if (nec->getNoteExpressionInfo(0, 0, i, info) == Steinberg::kResultOk &&
                info.typeId == SV::kTuningTypeID)
                caps.noteExpression = true;
        }
        nec->release();
    }

    // VST3's own bridge for MPE controllers: the plugin names the type it
    // wants for X, Y and pressure.
    raw = nullptr;
    if (ec->queryInterface(SV::INoteExpressionPhysicalUIMapping::iid, &raw) ==
            Steinberg::kResultOk && raw != nullptr) {
        auto* pui = static_cast<SV::INoteExpressionPhysicalUIMapping*>(raw);
        SV::PhysicalUIMap map[3] = {{SV::kPUIXMovement, SV::kInvalidTypeID},
                                    {SV::kPUIYMovement, SV::kInvalidTypeID},
                                    {SV::kPUIPressure, SV::kInvalidTypeID}};
        SV::PhysicalUIMapList list{3, map};
        if (pui->getPhysicalUIMapping(0, 0, list) == Steinberg::kResultOk) {
            if (map[0].noteExpressionTypeID != SV::kInvalidTypeID) {
                caps.pitchType = map[0].noteExpressionTypeID;
                caps.noteExpression = true;
            }
            if (map[1].noteExpressionTypeID != SV::kInvalidTypeID)
                caps.timbreType = map[1].noteExpressionTypeID;
            if (map[2].noteExpressionTypeID != SV::kInvalidTypeID)
                caps.pressureType = map[2].noteExpressionTypeID;
        }
        pui->release();
    }

    raw = nullptr;
    if (ec->queryInterface(SV::IMidiMapping::iid, &raw) == Steinberg::kResultOk &&
        raw != nullptr) {
        auto* mm = static_cast<SV::IMidiMapping*>(raw);
        auto mapped = [mm](Steinberg::int16 ch, SV::CtrlNumber ctrl) {
            SV::ParamID id = SV::kNoParamId;
            return mm->getMidiControllerAssignment(0, ch, ctrl, id) == Steinberg::kResultTrue
                       ? static_cast<std::uint32_t>(id) : engine::kNoParam;
        };
        for (Steinberg::int16 ch = 0; ch < 16; ++ch) {
            const auto c = static_cast<std::size_t>(ch);
            caps.bendParam[c]     = mapped(ch, SV::kPitchBend);
            caps.pressureParam[c] = mapped(ch, SV::kAfterTouch);
            caps.timbreParam[c]   = mapped(ch, static_cast<SV::CtrlNumber>(engine::kCtrlTimbre));
        }
        caps.rpnParam[0] = mapped(0, 101);
        caps.rpnParam[1] = mapped(0, 100);
        caps.rpnParam[2] = mapped(0, 6);
        mm->release();
    }

    ec->release();
    return caps;
}

}  // namespace

Vst3Device::Vst3Device(std::unique_ptr<juce::AudioPluginInstance> inst, DeviceIdentity id)
    : inst_(std::move(inst)), id_(std::move(id)) {
    if (inst_ != nullptr) {
        inst_->addListener(this);
        // Here and only here: the description allocates, and eventFlow() is
        // on the audio thread (ADR-0091).
        instrument_ = inst_->getPluginDescription().isInstrument;
        readParameters();
        // ADR-0097: once, here, on the message thread -- IMidiMapping and the
        // note-expression interfaces belong to the controller, which is not
        // called from the audio thread.
        caps_ = probeExpressionCaps(*inst_);
    }
    router_.configure(engine::resolveRoute(engine::RouteChoice::Auto, caps_), caps_);
    routeInUse_.store(static_cast<std::uint8_t>(router_.current()), std::memory_order_release);
}

Vst3Device::~Vst3Device() {
    if (inst_ != nullptr) inst_->removeListener(this);
}

void Vst3Device::readParameters() {
    params_.clear();
    handles_.clear();
    for (auto* p : inst_->getParameters()) {
        if (p == nullptr) continue;
        ParamDescriptor d;
        d.id   = paramIdToText(p);
        d.name = p->getName(128).toStdString();
        d.unit = p->getLabel().toStdString();

        // THE VST3 EDGE, and the reason ParamValue has `hasReal` at all.
        // VST3 exposes a real value only through getParamStringByValue, which
        // hands back a localised display string -- "4.80 kHz", "-inf dB",
        // "1/4 D". Parsing that back into a number is a guess that fails
        // differently per plugin and per locale, so the domain is normalized
        // and `real` is absent. That is a fact about VST3, not a gap in this
        // adapter: CLAP, Pd and native devices fill it in.
        d.domain = ParamDomain::Normalized;
        d.defaultValue = ParamValue::fromNormalized(
            static_cast<double>(p->getDefaultValue()));
        d.automatable = p->isAutomatable();
        params_.push_back(std::move(d));
        handles_.push_back(p);
    }
}

const ParamDescriptor* Vst3Device::paramAt(std::int32_t i) const noexcept {
    if (i < 0 || i >= static_cast<std::int32_t>(params_.size())) return nullptr;
    return &params_[static_cast<std::size_t>(i)];
}

ParamValue Vst3Device::getParam(const std::string& paramId) const noexcept {
    for (std::size_t i = 0; i < params_.size(); ++i) {
        if (params_[i].id != paramId) continue;
        auto* h = handles_[i];
        if (h == nullptr) return {};
        return ParamValue::fromNormalized(static_cast<double>(h->getValue()));
    }
    return {};
}

bool Vst3Device::setParam(const std::string& paramId, const ParamValue& v) {
    for (std::size_t i = 0; i < params_.size(); ++i) {
        if (params_[i].id != paramId) continue;
        auto* h = handles_[i];
        if (h == nullptr) return false;

        // SPEC 7.3: the gesture boundary is what makes one user movement one
        // undo step. beginChangeGesture/endChangeGesture are JUCE's spelling
        // of VST3's beginEdit/endEdit, and a setValueNotifyingHost without
        // them is a change the plugin's own automation recording never sees.
        h->beginChangeGesture();
        h->setValueNotifyingHost(static_cast<float>(v.normalized));
        h->endChangeGesture();
        return true;
    }
    return false;
}

void Vst3Device::prepare(double sampleRate, std::int32_t maxFrames) {
    if (inst_ == nullptr) return;

    const std::int32_t chans = std::max(inst_->getTotalNumInputChannels(),
                                        inst_->getTotalNumOutputChannels());

    // ALREADY RUNNING AND NOTHING CHANGED: DO NOTHING. The same rule as
    // `ClapDevice::prepare`, for the same reason and with the same evidence
    // behind it -- ADR-0089 prepares a whole new graph on every rebuild, and
    // `Graph::prepare` calls `prepare` on every node, so without this one
    // plugin's port rescan resets every plugin in the project. Measured
    // through the CLAP side of the same device contract: 5120 samples of
    // silence, 106.7 ms, from a Pro-Q 3 that had nothing to do with the
    // rescan.
    //
    // `prepareToPlay` is not a cheap no-op on a JUCE VST3 wrapper: it runs
    // `setupProcessing` and reactivates the component, which is exactly the
    // state loss this is here to avoid.
    if (prepared_ && sampleRate == sampleRate_ && maxFrames == maxFrames_ &&
        (chans < 1 ? 2 : chans) == channels_) {
        return;
    }

    maxFrames_ = maxFrames;
    sampleRate_ = sampleRate;
    channels_ = chans;
    if (channels_ < 1) channels_ = 2;

    // ADR-0049: the GRANTED size. Everything allocated here is sized from what
    // the driver returned, never from what was requested, and the plugin is
    // told the same number the graph will actually hand it.
    //
    // NOT setPlayConfigDetails, and this cost a JUCE assertion to find.
    // That function calls `disableNonMainBuses()` -- its own comment says "the
    // user does not want any side-buses or aux outputs" -- so it would have
    // silently switched off the sidechain input on every plugin that has one.
    // ADR-0043 requires a LIVE SIDECHAIN to prevent suspension and ADR-0056
    // added `Bus::Sidechain` to express it, so a compressor keyed from another
    // track would have had its key input disabled by the host, at prepare,
    // with nothing reported. The plugin's own bus layout is left alone and
    // only the rate and block size are set.
    inst_->setRateAndBufferSizeDetails(sampleRate, maxFrames);
    inst_->prepareToPlay(sampleRate, maxFrames);
    ++activations_;

    // ADR-0073's takeover. JUCE has now activated the component and set
    // processing; what it cannot do is carry note expression, because its
    // only event path is a MidiBuffer with noteId hardcoded to -1. So we
    // reach past it for `process` alone and leave discovery, state and
    // parameters where they are.
    processor_ = nullptr;
    if (const auto* client = inst_->getVST3Client()) {
        if (auto* comp = client->getIComponentPtr()) {
            void* raw = nullptr;
            if (comp->queryInterface(Steinberg::Vst::IAudioProcessor::iid, &raw)
                    == Steinberg::kResultOk && raw != nullptr) {
                processor_ = static_cast<Steinberg::Vst::IAudioProcessor*>(raw);
            }
        }
    }

    // Sized from the GRANTED block (ADR-0049) and allocated here, never in
    // process (ADR-0010). Capacity is ADR-0056's arithmetic: a 500 Hz MPE+
    // stream across this block, three dimensions, at polyphony 16.
    const double updates = (sampleRate > 0.0)
                             ? (static_cast<double>(maxFrames) / sampleRate) * 500.0 : 1.0;
    const auto perNote = static_cast<std::int32_t>(updates * 3.0) + 2;
    events_.reserve((perNote > 0 ? perNote : 8) * 16);
    // ADR-0097: every queue holds a block's worth of one dimension's stream,
    // which is what MpeMidi's IMidiMapping path puts into one parameter.
    const std::int32_t points = (perNote > 0 ? perNote : 8) + 8;
    paramChanges_.reserve(256, points);
    outParams_.reserve(256, points);
    // The router can turn one note-on into four outputs (three channel resets
    // and the note), plus the Configuration Message once.
    const std::int32_t routedCap = (perNote > 0 ? perNote : 8) * 16 + 16 * 4 + 8;
    routedStore_.assign(static_cast<std::size_t>(routedCap), engine::MpeOut{});
    routed_ = engine::MpeOutList(routedStore_.data(), routedCap);
    router_.reset();
    injected_.assign(static_cast<std::size_t>((perNote > 0 ? perNote : 8) * 16),
                     engine::Event{});
    injectedUsed_ = 0;
    rawIn_.resize(static_cast<std::size_t>(channels_));
    rawOut_.resize(static_cast<std::size_t>(channels_));

    // Allocated at prepare, never in process (ADR-0010).
    scratch_.setSize(channels_, maxFrames, false, true, true);
    midi_.ensureSize(4096);

    // HERE, and it was not. Since ADR-0073 this line sat after the `return`
    // in pushEvent -- unreachable -- so the guard at the top of this function
    // never fired and every prepare re-activated the plugin, the state loss
    // that guard exists to prevent. MSVC's C4702 found it in a -Werror JUCE
    // build; CI's JUCE job builds without -Werror (ADR-0097).
    prepared_ = true;
}

bool Vst3Device::pushEvent(const engine::Event& e) noexcept {
    if (injectedUsed_ >= injected_.size()) return false;
    injected_[injectedUsed_++] = e;
    return true;
}

void Vst3Device::release() {
    prepared_ = false;
    if (inst_ != nullptr) inst_->releaseResources();
}

void Vst3Device::process(const engine::NodeIo& io) noexcept {
    if (inst_ == nullptr || io.out == nullptr) return;

    const std::int32_t n = std::min(io.frames, maxFrames_);
    const std::int32_t ch = std::min(io.channels, channels_);
    if (n <= 0 || ch <= 0) return;

    // Copy in, process in place, copy out. JUCE wants one interleaved-by-
    // channel AudioBuffer for both directions; the graph hands separate in and
    // out pointers (ADR-0045's port pair), so the bridging happens here and
    // not in the graph.
    // `io.in` and `io.out` address the BLOCK; `io.frames` is this segment's
    // length and `io.blockOffset` is where it starts (ADR-0042). `scratch_` is
    // segment-sized and starts at 0, so the offset applies on the graph's side.
    const std::int32_t off = io.blockOffset;
    for (std::int32_t c = 0; c < ch; ++c) {
        float* dst = scratch_.getWritePointer(c);
        const float* src = (io.in != nullptr) ? io.in[c] + off : nullptr;
        if (src != nullptr) std::copy(src, src + n, dst);
        else                std::fill(dst, dst + n, 0.0f);
    }

    // --- ADR-0073: the raw path, where note expression actually works ----
    if (processor_ != nullptr) {
        events_.clear();
        paramChanges_.clear();
        outParams_.clear();

        // ADR-0097: a route change asked for since the last call. Every
        // sounding note ends on its own channel before the new route starts.
        routed_.clear();
        const auto want = requestedRoute_.load(std::memory_order_acquire);
        if (want != appliedRoute_) {
            router_.switchTo(engine::resolveRoute(static_cast<engine::RouteChoice>(want), caps_),
                             off, routed_);
            appliedRoute_ = want;
            routeInUse_.store(static_cast<std::uint8_t>(router_.current()),
                              std::memory_order_release);
        }

        // The route decides what the plugin receives: channel 0 and note
        // expression, member channels and channel messages, or plain MIDI.
        router_.route(io.events.first, io.events.count, off, routed_);
        router_.route(injected_.data(), static_cast<std::int32_t>(injectedUsed_), off, routed_);
        injectedUsed_ = 0;

        // ADR-0081: Event::frame is BLOCK-relative and the plugin is handed
        // one segment, so the offset comes off here and a mismatch is
        // counted rather than silently refused. A channel message the plugin
        // mapped to a parameter rides IParameterChanges in this same call
        // (ADR-0073); everything else is an event.
        for (const engine::MpeOut& o : routed_) {
            if (o.kind == engine::MpeOut::Kind::Control && o.paramId != engine::kNoParam) {
                const std::int32_t t = o.frame - off;
                if (t < 0 || t >= n) { ++routedOutOfRange_; continue; }
                paramChanges_.set(o.paramId, o.value, t);
            } else {
                events_.addOut(o, off, n);
            }
        }

        for (std::int32_t c = 0; c < ch; ++c) {
            rawIn_[static_cast<std::size_t>(c)]  = scratch_.getWritePointer(c);
            rawOut_[static_cast<std::size_t>(c)] = scratch_.getWritePointer(c);
        }

        Steinberg::Vst::AudioBusBuffers inBus{};
        inBus.numChannels = ch;
        inBus.silenceFlags = 0;
        inBus.channelBuffers32 = rawIn_.data();
        Steinberg::Vst::AudioBusBuffers outBus = inBus;
        outBus.channelBuffers32 = rawOut_.data();

        Steinberg::Vst::ProcessData pd{};
        pd.processMode = Steinberg::Vst::kRealtime;
        pd.symbolicSampleSize = Steinberg::Vst::kSample32;
        pd.numSamples = n;
        pd.numInputs = 1;
        pd.numOutputs = 1;
        pd.inputs = &inBus;
        pd.outputs = &outBus;
        pd.inputEvents = &events_;
        pd.outputEvents = nullptr;
        pd.inputParameterChanges = &paramChanges_;
        pd.outputParameterChanges = &outParams_;
        pd.processContext = nullptr;

        const Steinberg::tresult r = processor_->process(pd);
        if (r != Steinberg::kResultOk) {
            // The plugin did not write the output, so copying it out hands
            // the graph last block's audio. Silence for this segment only.
            for (std::int32_t c = 0; c < io.channels; ++c)
                if (io.out[c] != nullptr)
                    for (std::int32_t i = 0; i < io.frames; ++i) io.out[c][i] = 0.0f;
            return;
        }

        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* out = io.out[c];
            if (out == nullptr) continue;
            out += off;
            if (c < ch) std::copy(rawOut_[static_cast<std::size_t>(c)],
                                  rawOut_[static_cast<std::size_t>(c)] + n, out);
            else        std::fill(out, out + n, 0.0f);
            for (std::int32_t i = n; i < io.frames; ++i) out[i] = 0.0f;
        }
        continuousTime_ += n;
        return;
    }

    midi_.clear();
    // THE EVENT PATH IS NOT WIRED, AND IT IS EMPTY RATHER THAN APPROXIMATE.
    //
    // Translating `engine::Event` into this MidiBuffer is the obvious next
    // line and it is deliberately not written. Every per-note expression value
    // would be quantised to 7 bits and anchored to a noteId JUCE hardcodes to
    // -1, so an instrument would play and MPE+ would silently not work -- the
    // exact shape ADR-0054 warns about, where nothing fails and the extra bits
    // simply stop arriving. An empty buffer is a plugin that makes no sound,
    // which is a bug report; a 7-bit buffer is a plugin that sounds nearly
    // right, which is not. ADR-0057 has the route.

    juce::AudioBuffer<float> view(scratch_.getArrayOfWritePointers(), ch, n);
    inst_->processBlock(view, midi_);

    for (std::int32_t c = 0; c < io.channels; ++c) {
        float* out = io.out[c];
        if (out == nullptr) continue;
        out += off;
        if (c < ch) std::copy(view.getReadPointer(c), view.getReadPointer(c) + n, out);
        else        std::fill(out, out + n, 0.0f);
        // A segment shorter than the buffer leaves the tail untouched, and the
        // tail is last block's audio.
        for (std::int32_t i = n; i < io.frames; ++i) out[i] = 0.0f;
    }
}

std::int64_t Vst3Device::tailSamples() const noexcept {
    if (inst_ == nullptr) return 0;
    const double secs = inst_->getTailLengthSeconds();

    // A branch, not arithmetic. JUCE reports an unbounded tail as infinity,
    // and `infinity * sampleRate` cast to int64 is undefined behaviour rather
    // than a large number -- which is how a "never suspend" declaration turns
    // into a node suspended on its first block.
    if (!std::isfinite(secs) || secs < 0.0) return engine::kInfiniteTail;

    const double rate = inst_->getSampleRate();
    if (rate <= 0.0) return engine::kInfiniteTail;   // not prepared yet: conservative

    const double samples = secs * rate;
    if (samples >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return engine::kInfiniteTail;
    return static_cast<std::int64_t>(samples);
}

std::int32_t Vst3Device::latencySamples() const noexcept {
    if (inst_ == nullptr) return 0;
    const int l = inst_->getLatencySamples();
    // A plugin reporting a negative latency is reporting nonsense, and
    // compensating by a negative number moves audio EARLIER than the source.
    return l > 0 ? l : 0;
}

std::vector<std::string> Vst3Device::stateRoles() const {
    if (inst_ == nullptr) return {};
    // One role today. JUCE's getStateInformation already merges VST3's
    // component and controller streams into one opaque block, so splitting
    // them here would invent a boundary we cannot honour on the way back in.
    // `plugin_state.stream_role` still admits both because the FORMAT must
    // represent a file another implementation wrote (SPEC 7).
    return {"chunk"};
}

std::vector<std::uint8_t> Vst3Device::saveState(const std::string& role) const {
    if (inst_ == nullptr || role != "chunk") return {};
    juce::MemoryBlock mb;
    inst_->getStateInformation(mb);
    const auto* p = static_cast<const std::uint8_t*>(mb.getData());
    return std::vector<std::uint8_t>(p, p + mb.getSize());
}

bool Vst3Device::loadState(const std::string& role, const std::vector<std::uint8_t>& b) {
    if (inst_ == nullptr || role != "chunk") return false;
    inst_->setStateInformation(b.data(), static_cast<int>(b.size()));
    return true;
}

void* Vst3Device::rawComponent() const noexcept {
    if (inst_ == nullptr) return nullptr;
    if (const auto* c = inst_->getVST3Client()) return c->getIComponentPtr();
    return nullptr;
}

void Vst3Device::audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& d) {
    // ADR-0066. REPORT AND RETURN. Steinberg calls restartComponent from
    // whichever thread the plugin felt like, and recomputing the compensated
    // schedule here -- on that thread, while it waits -- is the bug the ADR
    // exists to prevent. A mode switch reports several times in milliseconds,
    // so the message thread coalesces these and republishes once.
    if (d.latencyChanged)
        latencyEpoch_.fetch_add(1, std::memory_order_release);
    if (d.parameterInfoChanged || d.programChanged || d.nonParameterStateChanged)
        stateEpoch_.fetch_add(1, std::memory_order_release);
}

}  // namespace adi::device
