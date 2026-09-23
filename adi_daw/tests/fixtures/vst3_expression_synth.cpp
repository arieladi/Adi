// SPDX-License-Identifier: GPL-3.0-or-later
//
// A test VST3 instrument whose EDIT CONTROLLER IS REACHABLE -- ADR-0100.
//
// Every MPE synth on the development machine hides its controller (JUCE builds
// the controller as a separate class and never gives a host the object), so the
// half of ADR-0097 that reads a controller -- `probeExpressionCaps`, `Auto`'s
// decisions, and MpeMidi's IMidiMapping PARAMETER path -- had never run against
// anything. This plugin is one class that is both component and controller,
// which is the one arrangement our host can see into, in three variants:
//
//   MPE       IMidiMapping: pitch bend, channel pressure and CC74 on EVERY
//             channel, each to a parameter of its own, plus CC101/100/6 on
//             channel 0 for the Configuration Message. No note expression.
//             It IGNORES kLegacyMIDICCOutEvent, so only the parameter path can
//             move it. Member-channel bend is +/-48 once an MCM has arrived,
//             +/-2 before -- a receiver that was never configured.
//   NoteExpr  INoteExpressionController lists Tuning and two CUSTOM types, and
//             the physical-UI mapping names them for Y and pressure. A host
//             using the defaults (Brightness, Expression) would move nothing.
//   Plain     IMidiMapping maps pitch bend on channel 0 only: a global bend,
//             which is not an MPE receiver. Poly aftertouch drives pressure.
//
// Each voice is a sine at the note's pitch, with pressure as amplitude
// (0.25 + 0.75 p) and timbre as a 3rd harmonic (0.05 + 0.95 t) -- chosen so a
// dimension that never arrives still sounds, and reads "unchanged".
//
// Built only against Steinberg's VST3 SDK (MIT, 2025), the copy JUCE carries.
// No JUCE code is used or linked.

#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstphysicalui.h>
#include <pluginterfaces/vst/ivstunits.h>
#include <pluginterfaces/vst/vsttypes.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

enum class Variant { Mpe, NoteExpr, Plain };

// Class ids. Arbitrary, fixed, and ours.
const TUID kMpeCid      = INLINE_UID(0xAD1E0001, 0x4D504531, 0x9F2A0C11, 0x00000001);
const TUID kNoteExprCid = INLINE_UID(0xAD1E0001, 0x4E4F5445, 0x9F2A0C11, 0x00000002);
const TUID kPlainCid    = INLINE_UID(0xAD1E0001, 0x504C4149, 0x9F2A0C11, 0x00000003);

constexpr NoteExpressionTypeID kTimbreType   = 100001;   ///< custom: named for PUI Y
constexpr NoteExpressionTypeID kPressureType = 100002;   ///< custom: named for PUI pressure

constexpr ParamID kBend = 1000, kPressure = 2000, kTimbre = 3000;   ///< + channel
constexpr ParamID kRpnMsb = 500, kRpnLsb = 501, kDataEntry = 502;    ///< channel 0
/// ADR-0110 d3: a knob the user can move inside the plugin's own window, and a
/// hidden switch a test flips to make the plugin perform that drag -- beginEdit,
/// forty performEdits, endEdit through the host's IComponentHandler -- exactly as
/// a real editor does. Drive also ECHOES a host set back with performEdit, as
/// some real plugins do, twice: at once (JUCE 9 hands a host set to the
/// controller synchronously, so this lands inside the host's own set) and
/// again when the deferred-echo switch is flipped (as a plugin's own timer
/// would), which is the case the host's echo guard exists for.
constexpr ParamID kDrive = 4000, kGestureTrigger = 4001, kDeferredEcho = 4002;
constexpr double kDriveDefault = 0.2, kDriveDragTarget = 0.7;
constexpr int kDriveDragSteps = 40;

constexpr double kPi = 3.14159265358979323846;

void copy128(String128 dst, const char* src) {
    int i = 0;
    for (; src[i] != 0 && i < 127; ++i) dst[i] = static_cast<char16>(src[i]);
    dst[i] = 0;
}

bool same(const TUID a, const TUID b) { return std::memcmp(a, b, sizeof(TUID)) == 0; }

/// A member-channel bend word over 16383 -- the scale ADR-0097 sends -- to
/// semitones at `range`: 8192 steps below centre, 8191 above.
double bendSemitones(double norm, double range) {
    const double w = norm * 16383.0;
    return w >= 8192.0 ? (w - 8192.0) / 8191.0 * range : (w - 8192.0) / 8192.0 * range;
}

struct Voice {
    bool on = false;
    int key = 0;
    int channel = 0;
    int32 noteId = -1;
    double phase = 0.0;
    // NoteExpr: per note. Plain: pressure per note. MPE: read from the channel.
    double tuning = 0.0, pressure = 0.0, timbre = 0.0;
};

class Synth final : public IComponent,
                    public IAudioProcessor,
                    public IEditController,
                    public IMidiMapping,
                    public INoteExpressionController,
                    public INoteExpressionPhysicalUIMapping {
public:
    explicit Synth(Variant v) : variant_(v) {
        for (auto& b : bend_) b = 0.5;
    }
    virtual ~Synth() = default;

    // --- FUnknown ----------------------------------------------------------
    tresult PLUGIN_API queryInterface(const TUID queried, void** obj) override {
        if (obj == nullptr) return kInvalidArgument;
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual(queried, FUnknown::iid) || FUnknownPrivate::iidEqual(queried, IPluginBase::iid) ||
            FUnknownPrivate::iidEqual(queried, IComponent::iid))
            *obj = static_cast<IComponent*>(this);
        else if (FUnknownPrivate::iidEqual(queried, IAudioProcessor::iid))
            *obj = static_cast<IAudioProcessor*>(this);
        else if (FUnknownPrivate::iidEqual(queried, IEditController::iid))
            *obj = static_cast<IEditController*>(this);
        else if (FUnknownPrivate::iidEqual(queried, IMidiMapping::iid) && variant_ != Variant::NoteExpr)
            *obj = static_cast<IMidiMapping*>(this);
        else if (FUnknownPrivate::iidEqual(queried, INoteExpressionController::iid) && variant_ == Variant::NoteExpr)
            *obj = static_cast<INoteExpressionController*>(this);
        else if (FUnknownPrivate::iidEqual(queried, INoteExpressionPhysicalUIMapping::iid) &&
                 variant_ == Variant::NoteExpr)
            *obj = static_cast<INoteExpressionPhysicalUIMapping*>(this);
        if (*obj == nullptr) return kNoInterface;
        addRef();
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // --- IPluginBase (shared by IComponent and IEditController) ------------
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { setComponentHandler(nullptr); return kResultOk; }

    // --- IComponent ------------------------------------------------------------
    tresult PLUGIN_API getControllerClassId(TUID) override { return kResultFalse; }   // we ARE it
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
        if (type == kAudio) return 1;                 // one stereo in, one stereo out
        if (type == kEvent && dir == kInput) return 1;
        return 0;
    }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (index != 0 || getBusCount(type, dir) == 0) return kInvalidArgument;
        bus.mediaType = type;
        bus.direction = dir;
        bus.channelCount = type == kAudio ? 2 : 16;
        copy128(bus.name, type == kAudio ? (dir == kInput ? "In" : "Out") : "Notes");
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }

    // --- IAudioProcessor -----------------------------------------------------
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement*, int32, SpeakerArrangement* outs,
                                          int32 numOuts) override {
        return (numOuts == 1 && outs != nullptr && outs[0] == SpeakerArr::kStereo) ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32 index, SpeakerArrangement& arr) override {
        if (index != 0) return kInvalidArgument;
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override {
        return size == kSample32 ? kResultTrue : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        sampleRate_ = setup.sampleRate > 0.0 ? setup.sampleRate : 48000.0;
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    uint32 PLUGIN_API getTailSamples() override { return 0; }

    tresult PLUGIN_API process(ProcessData& data) override {
        applyParameters(data.inputParameterChanges);
        applyEvents(data.inputEvents);
        render(data);
        return kResultOk;
    }

    // --- IEditController ------------------------------------------------------
    tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
    /// The expression parameters each variant had before ADR-0110's two were added.
    int32 baseParameterCount() const {
        if (variant_ == Variant::Mpe) return 16 * 3 + 3;
        if (variant_ == Variant::Plain) return 1;
        return 0;
    }
    int32 PLUGIN_API getParameterCount() override { return baseParameterCount() + 3; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        if (index < 0 || index >= getParameterCount()) return kInvalidArgument;
        info = ParameterInfo{};
        char name[64];
        if (index >= baseParameterCount()) {
            const int32 k = index - baseParameterCount();
            const bool drive = k == 0;
            info.id = drive ? kDrive : (k == 1 ? kGestureTrigger : kDeferredEcho);
            copy128(info.title, drive ? "Drive" : (k == 1 ? "Gesture Trigger" : "Deferred Echo"));
            copy128(info.shortTitle, drive ? "Drive" : (k == 1 ? "Trigger" : "Echo"));
            info.defaultNormalizedValue = drive ? kDriveDefault : 0.0;
            info.unitId = kRootUnitId;
            info.flags = drive ? ParameterInfo::kCanAutomate : ParameterInfo::kIsHidden;
            return kResultOk;
        }
        if (index < 48) {
            const int kind = index / 16, ch = index % 16;
            const ParamID base = kind == 0 ? kBend : (kind == 1 ? kPressure : kTimbre);
            info.id = base + static_cast<ParamID>(ch);
            std::snprintf(name, sizeof name, "%s %d", kind == 0 ? "Bend" : (kind == 1 ? "Pressure" : "CC74"), ch + 1);
            info.defaultNormalizedValue = kind == 0 ? 0.5 : 0.0;
        } else {
            info.id = kRpnMsb + static_cast<ParamID>(index - 48);
            std::snprintf(name, sizeof name, "CC%d", index == 48 ? 101 : (index == 49 ? 100 : 6));
        }
        copy128(info.title, name);
        copy128(info.shortTitle, name);
        info.unitId = kRootUnitId;
        info.flags = ParameterInfo::kIsHidden;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue v, String128 s) override {
        char b[32];
        std::snprintf(b, sizeof b, "%.4f", v);
        copy128(s, b);
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kResultFalse; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override {
        return id == kDrive ? drive_ : 0.0;
    }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override {
        if (id == kDrive) {
            const bool changed = std::fabs(v - drive_) > 1e-9;
            drive_ = v;
            // The echo: tell the host about the value it just set. Harmless to a
            // careful host, an extra undo step to a careless one.
            if (changed && !dragging_) {
                pendingEcho_ = v;
                hasPendingEcho_ = true;
                if (handler_ != nullptr) handler_->performEdit(kDrive, v);
            }
            return kResultOk;
        }
        if (id == kDeferredEcho && v > 0.5 && hasPendingEcho_ && handler_ != nullptr) {
            hasPendingEcho_ = false;
            handler_->performEdit(kDrive, pendingEcho_);
            return kResultOk;
        }
        if (id == kGestureTrigger && v > 0.5 && handler_ != nullptr && !dragging_) {
            // The user's drag, as an editor reports it.
            dragging_ = true;
            const double from = drive_;
            handler_->beginEdit(kDrive);
            for (int i = 1; i <= kDriveDragSteps; ++i) {
                drive_ = from + (kDriveDragTarget - from) * i / kDriveDragSteps;
                handler_->performEdit(kDrive, drive_);
            }
            handler_->endEdit(kDrive);
            dragging_ = false;
        }
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler* h) override {
        if (h == handler_) return kResultOk;
        if (handler_ != nullptr) handler_->release();
        handler_ = h;
        if (handler_ != nullptr) handler_->addRef();
        return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

    // --- IMidiMapping --------------------------------------------------------
    tresult PLUGIN_API getMidiControllerAssignment(int32 bus, int16 channel, CtrlNumber ctrl,
                                                   ParamID& id) override {
        if (bus != 0 || channel < 0 || channel > 15) return kResultFalse;
        if (variant_ == Variant::Plain) {
            if (channel == 0 && ctrl == kPitchBend) { id = kBend; return kResultTrue; }
            return kResultFalse;
        }
        const auto ch = static_cast<ParamID>(channel);
        if (ctrl == kPitchBend)  { id = kBend + ch; return kResultTrue; }
        if (ctrl == kAfterTouch) { id = kPressure + ch; return kResultTrue; }
        if (ctrl == 74)          { id = kTimbre + ch; return kResultTrue; }
        if (channel == 0 && ctrl == 101) { id = kRpnMsb; return kResultTrue; }
        if (channel == 0 && ctrl == 100) { id = kRpnLsb; return kResultTrue; }
        if (channel == 0 && ctrl == 6)   { id = kDataEntry; return kResultTrue; }
        return kResultFalse;
    }

    // --- INoteExpressionController ---------------------------------------------
    int32 PLUGIN_API getNoteExpressionCount(int32 bus, int16) override { return bus == 0 ? 3 : 0; }
    tresult PLUGIN_API getNoteExpressionInfo(int32 bus, int16, int32 index, NoteExpressionTypeInfo& info) override {
        if (bus != 0 || index < 0 || index > 2) return kInvalidArgument;
        info = NoteExpressionTypeInfo{};
        const NoteExpressionTypeID ids[3] = {kTuningTypeID, kTimbreType, kPressureType};
        const char* names[3] = {"Tuning", "Timbre", "Pressure"};
        info.typeId = ids[index];
        copy128(info.title, names[index]);
        copy128(info.shortTitle, names[index]);
        info.valueDesc.defaultValue = index == 0 ? 0.5 : 0.0;
        info.valueDesc.minimum = 0.0;
        info.valueDesc.maximum = 1.0;
        info.associatedParameterId = kNoParamId;
        info.flags = index == 0 ? NoteExpressionTypeInfo::kIsBipolar : 0;
        return kResultOk;
    }
    tresult PLUGIN_API getNoteExpressionStringByValue(int32, int16, NoteExpressionTypeID, NoteExpressionValue v,
                                                      String128 s) override {
        char b[32];
        std::snprintf(b, sizeof b, "%.4f", v);
        copy128(s, b);
        return kResultOk;
    }
    tresult PLUGIN_API getNoteExpressionValueByString(int32, int16, NoteExpressionTypeID, const TChar*,
                                                      NoteExpressionValue&) override {
        return kResultFalse;
    }

    // --- INoteExpressionPhysicalUIMapping --------------------------------------
    tresult PLUGIN_API getPhysicalUIMapping(int32 bus, int16, PhysicalUIMapList& list) override {
        if (bus != 0) return kResultFalse;
        for (uint32 i = 0; i < list.count; ++i) {
            PhysicalUIMap& m = list.map[i];
            if (m.physicalUITypeID == kPUIXMovement)      m.noteExpressionTypeID = kTuningTypeID;
            else if (m.physicalUITypeID == kPUIYMovement) m.noteExpressionTypeID = kTimbreType;
            else if (m.physicalUITypeID == kPUIPressure)  m.noteExpressionTypeID = kPressureType;
            else                                          m.noteExpressionTypeID = kInvalidTypeID;
        }
        return kResultOk;
    }

private:
    void applyParameters(IParameterChanges* changes) {
        if (changes == nullptr) return;
        const int32 n = changes->getParameterCount();
        for (int32 i = 0; i < n; ++i) {
            IParamValueQueue* q = changes->getParameterData(i);
            if (q == nullptr) continue;
            const ParamID id = q->getParameterId();
            const int32 points = q->getPointCount();
            for (int32 p = 0; p < points; ++p) {
                int32 offset = 0;
                ParamValue v = 0.0;
                if (q->getPoint(p, offset, v) != kResultOk) continue;
                setParameter(id, v);
            }
        }
    }

    void setParameter(ParamID id, double v) {
        if (id >= kBend && id < kBend + 16)         bend_[id - kBend] = v;
        else if (id >= kPressure && id < kPressure + 16) pressure_[id - kPressure] = v;
        else if (id >= kTimbre && id < kTimbre + 16)     timbre_[id - kTimbre] = v;
        else if (id == kRpnMsb)   rpnMsb_ = static_cast<int>(std::lround(v * 127.0));
        else if (id == kRpnLsb)   rpnLsb_ = static_cast<int>(std::lround(v * 127.0));
        else if (id == kDataEntry && rpnMsb_ == 0 && rpnLsb_ == 6)
            memberBend_ = std::lround(v * 127.0) > 0 ? 48.0 : 2.0;   // the MCM: MPE's +/-48
    }

    Voice* voiceFor(int32 noteId, int key, int channel) {
        for (auto& v : voices_)
            if (v.on && (noteId >= 0 ? v.noteId == noteId : (v.key == key && v.channel == channel))) return &v;
        return nullptr;
    }

    void applyEvents(IEventList* events) {
        if (events == nullptr) return;
        const int32 n = events->getEventCount();
        for (int32 i = 0; i < n; ++i) {
            Event e{};
            if (events->getEvent(i, e) != kResultOk) continue;
            switch (e.type) {
                case Event::kNoteOnEvent: {
                    Voice* v = nullptr;
                    for (auto& c : voices_) if (!c.on) { v = &c; break; }
                    if (v == nullptr) break;
                    *v = Voice{};
                    v->on = true;
                    v->key = e.noteOn.pitch;
                    v->channel = e.noteOn.channel;
                    v->noteId = e.noteOn.noteId;
                    break;
                }
                case Event::kNoteOffEvent:
                    if (Voice* v = voiceFor(e.noteOff.noteId, e.noteOff.pitch, e.noteOff.channel)) v->on = false;
                    break;
                case Event::kPolyPressureEvent:
                    if (variant_ == Variant::Plain)
                        for (auto& v : voices_)
                            if (v.on && v.key == e.polyPressure.pitch) v.pressure = e.polyPressure.pressure;
                    break;
                case Event::kNoteExpressionValueEvent:
                    if (variant_ == Variant::NoteExpr)
                        for (auto& v : voices_) {
                            if (!v.on || v.noteId != e.noteExpressionValue.noteId) continue;
                            const double x = e.noteExpressionValue.value;
                            if (e.noteExpressionValue.typeId == kTuningTypeID) v.tuning = 240.0 * (x - 0.5);
                            else if (e.noteExpressionValue.typeId == kTimbreType) v.timbre = x;
                            else if (e.noteExpressionValue.typeId == kPressureType) v.pressure = x;
                        }
                    break;
                default:
                    break;      // kLegacyMIDICCOutEvent included: this plugin reads MIDI only as parameters
            }
        }
    }

    void render(ProcessData& data) {
        if (data.numOutputs < 1 || data.outputs == nullptr || data.outputs[0].channelBuffers32 == nullptr) return;
        AudioBusBuffers& out = data.outputs[0];
        const int32 frames = data.numSamples;
        for (int32 c = 0; c < out.numChannels; ++c)
            if (out.channelBuffers32[c] != nullptr) std::memset(out.channelBuffers32[c], 0, sizeof(float) * static_cast<size_t>(frames));
        for (auto& v : voices_) {
            if (!v.on) continue;
            const auto ch = static_cast<size_t>(v.channel & 15);
            double semis = 0.0, p = 0.0, t = 0.0;
            if (variant_ == Variant::Mpe) {
                semis = bendSemitones(bend_[ch], memberBend_);
                p = pressure_[ch];
                t = timbre_[ch];
            } else {
                semis = v.tuning;
                p = v.pressure;
                t = v.timbre;
            }
            const double hz = 440.0 * std::pow(2.0, (v.key + semis - 69.0) / 12.0);
            const double step = 2.0 * kPi * hz / sampleRate_;
            const double amp = 0.3 * (0.25 + 0.75 * p);
            const double third = 0.05 + 0.95 * t;
            for (int32 i = 0; i < frames; ++i) {
                const auto s = static_cast<float>(amp * (std::sin(v.phase) + third * std::sin(3.0 * v.phase)));
                for (int32 c = 0; c < out.numChannels; ++c)
                    if (out.channelBuffers32[c] != nullptr) out.channelBuffers32[c][i] += s;
                v.phase += step;
                if (v.phase > 2.0 * kPi) v.phase -= 2.0 * kPi;
            }
        }
    }

    Variant variant_;
    std::atomic<uint32> refs_{1};
    IComponentHandler* handler_ = nullptr;   ///< the host's, held with a reference
    double drive_ = kDriveDefault;
    bool dragging_ = false;
    double pendingEcho_ = 0.0;
    bool hasPendingEcho_ = false;
    double sampleRate_ = 48000.0;
    std::array<Voice, 16> voices_{};
    std::array<double, 16> bend_{}, pressure_{}, timbre_{};
    int rpnMsb_ = 127, rpnLsb_ = 127;
    double memberBend_ = 2.0;          ///< +/-2 until a Configuration Message arrives
};

class Factory final : public IPluginFactory2 {
public:
    tresult PLUGIN_API queryInterface(const TUID queried, void** obj) override {
        if (obj == nullptr) return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(queried, FUnknown::iid) || FUnknownPrivate::iidEqual(queried, IPluginFactory::iid) ||
            FUnknownPrivate::iidEqual(queried, IPluginFactory2::iid)) {
            *obj = static_cast<IPluginFactory2*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }      // static: lives as long as the module
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        if (info == nullptr) return kInvalidArgument;
        *info = PFactoryInfo{};
        std::snprintf(info->vendor, sizeof info->vendor, "adi_daw tests");
        std::snprintf(info->url, sizeof info->url, "https://github.com/arieladi/Adi");
        info->flags = PFactoryInfo::kNoFlags;
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 3; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        PClassInfo2 i2{};
        if (getClassInfo2(index, &i2) != kResultOk || info == nullptr) return kInvalidArgument;
        *info = PClassInfo{};
        std::memcpy(info->cid, i2.cid, sizeof(TUID));
        info->cardinality = i2.cardinality;
        std::memcpy(info->category, i2.category, sizeof info->category);
        std::memcpy(info->name, i2.name, sizeof info->name);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        if (info == nullptr || index < 0 || index > 2) return kInvalidArgument;
        *info = PClassInfo2{};
        const TUID* cids[3] = {&kMpeCid, &kNoteExprCid, &kPlainCid};
        const char* names[3] = {"ADI Test MPE", "ADI Test NoteExpr", "ADI Test Plain"};
        std::memcpy(info->cid, *cids[index], sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        std::snprintf(info->category, sizeof info->category, "%s", kVstAudioEffectClass);
        std::snprintf(info->name, sizeof info->name, "%s", names[index]);
        std::snprintf(info->subCategories, sizeof info->subCategories, "Instrument|Synth");
        std::snprintf(info->vendor, sizeof info->vendor, "adi_daw tests");
        std::snprintf(info->version, sizeof info->version, "1.0.0");
        std::snprintf(info->sdkVersion, sizeof info->sdkVersion, "%s", kVstVersionString);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString wanted, void** obj) override {
        if (obj == nullptr) return kInvalidArgument;
        *obj = nullptr;
        TUID want;
        std::memcpy(want, cid, sizeof(TUID));
        Synth* s = nullptr;
        if (same(want, kMpeCid)) s = new Synth(Variant::Mpe);
        else if (same(want, kNoteExprCid)) s = new Synth(Variant::NoteExpr);
        else if (same(want, kPlainCid)) s = new Synth(Variant::Plain);
        if (s == nullptr) return kNoInterface;
        TUID asked;
        std::memcpy(asked, wanted, sizeof(TUID));
        const tresult r = s->queryInterface(asked, obj);
        s->release();                  // the query holds the reference now, or nothing does
        return r;
    }
};

}  // namespace

extern "C" SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() {
    static Factory factory;
    return &factory;
}
