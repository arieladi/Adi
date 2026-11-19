// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/digest.hpp"
#include "adi/engine/param_ops.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/ui/project_document.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace { std::atomic<unsigned> callbackAllocations{0}; }
void* operator new(std::size_t n) {
    if (adi::audio::onAudioThread) ++callbackAllocations;
    if (auto* pointer = std::malloc(n ? n : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return operator new[](n); } catch (...) { return nullptr; }
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }

void* operator new(std::size_t n, std::align_val_t alignment) {
    if (adi::audio::onAudioThread) ++callbackAllocations;
    const auto boundary = static_cast<std::size_t>(alignment);
    const auto bytes = n ? n : 1;
    if (boundary == 0 || bytes > std::numeric_limits<std::size_t>::max() - (boundary - 1)) throw std::bad_alloc();
#if defined(_MSC_VER)
    if (auto* pointer = _aligned_malloc(bytes, boundary)) return pointer;
#else
    const auto rounded = ((bytes + boundary - 1) / boundary) * boundary;
    if (auto* pointer = std::aligned_alloc(boundary, rounded)) return pointer;
#endif
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t alignment) { return operator new(n, alignment); }
void* operator new(std::size_t n, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return operator new(n, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return operator new[](n, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, std::align_val_t) noexcept {
#if defined(_MSC_VER)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept { operator delete(pointer, alignment); }
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept { operator delete(pointer, alignment); }
void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept { operator delete(pointer, alignment); }
void operator delete(void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept { operator delete(pointer, alignment); }
void operator delete[](void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept { operator delete(pointer, alignment); }

namespace {
using namespace adi;
using namespace adi::engine;
int checks = 0, failures = 0;
void check(bool value, const std::string& name) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", name.c_str()); }
}
void require(bool value, const std::string& name) {
    if (!value) throw std::runtime_error(name);
}
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Patch {
    double gain = 0.5, bias = 0;
    std::uint64_t number = 0;
    bool operator==(const Patch&) const = default;
};
std::vector<std::uint8_t> patchBytes(Patch patch) {
    std::vector<std::uint8_t> bytes(24);
    std::memcpy(bytes.data(), &patch.gain, sizeof patch.gain);
    std::memcpy(bytes.data() + 8, &patch.bias, sizeof patch.bias);
    std::memcpy(bytes.data() + 16, &patch.number, sizeof patch.number);
    return bytes;
}
std::optional<Patch> readPatch(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 24) return std::nullopt;
    Patch patch;
    std::memcpy(&patch.gain, bytes.data(), sizeof patch.gain);
    std::memcpy(&patch.bias, bytes.data() + 8, sizeof patch.bias);
    std::memcpy(&patch.number, bytes.data() + 16, sizeof patch.number);
    if (!std::isfinite(patch.gain) || patch.gain < 0 || patch.gain > 1 ||
        !std::isfinite(patch.bias) || patch.bias < 0 || patch.bias > 1) return std::nullopt;
    return patch;
}
enum class CallKind { Parameter, State };
struct Call {
    CallKind kind = CallKind::Parameter;
    int parameter = -1;
    Patch value;
    bool accepted = false;
};
struct Probe {
    std::array<Call, 512> calls{};
    std::size_t count = 0;
    unsigned loads = 0, sets = 0, attempts = 0, destroyed = 0;
    unsigned floatCalls = 0, doubleCalls = 0;
    bool overflow = false, ownerCallOnAudio = false;
    std::uint64_t serial = 0;
    void add(Call call) noexcept {
        if (count < calls.size()) calls[count++] = call;
        else overflow = true;
    }
};

// Actual processing reads the accepted runtime values. The output oracle never
// reads SQL, ParamOps mirrors or this fake's getters to obtain its expectation.
class RuntimeDevice final : public device::DeviceInstance {
public:
    explicit RuntimeDevice(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {
        identity_.format = "vst3"; identity_.uid = "test.param.live"; identity_.name = "Runtime probe";
        descriptors_[0].id = "gain"; descriptors_[0].name = "Gain";
        descriptors_[0].defaultValue = device::ParamValue::fromNormalized(0.5);
        descriptors_[1].id = "bias"; descriptors_[1].name = "Bias";
        descriptors_[1].defaultValue = device::ParamValue::fromNormalized(0);
    }
    ~RuntimeDevice() override { ++probe_->destroyed; }
    const device::DeviceIdentity& identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    bool processesDouble() const noexcept override { return true; }
    std::int64_t tailSamples() const noexcept override { return kInfiniteTail; }
    std::int32_t paramCount() const noexcept override { return 2; }
    const device::ParamDescriptor* paramAt(std::int32_t index) const noexcept override {
        return index >= 0 && index < 2 ? &descriptors_[static_cast<std::size_t>(index)] : nullptr;
    }
    device::ParamValue getParam(const std::string& id) const noexcept override {
        const int index = indexOf(id);
        return index >= 0 ? device::ParamValue::fromNormalized(values_[static_cast<std::size_t>(index)].load()) : device::ParamValue{};
    }
    bool setParam(const std::string& id, const device::ParamValue& value) override {
        probe_->ownerCallOnAudio = probe_->ownerCallOnAudio || audio::onAudioThread;
        ++probe_->attempts;
        const int index = indexOf(id);
        const bool accepted = index >= 0 && std::isfinite(value.normalized) && value.normalized >= 0 &&
            value.normalized <= 1 && !refuseParameters && index != refuseParameter &&
            (!refuseParameterInPatch || patch_ != *refuseParameterInPatch);
        Patch observation = patch();
        if (index == 0) observation.gain = value.normalized;
        if (index == 1) observation.bias = value.normalized;
        probe_->add({CallKind::Parameter, index, observation, accepted});
        if (!accepted) return false;
        ++probe_->sets;
        const auto at = static_cast<std::size_t>(index);
        if (deferred) { pending_[at] = value.normalized; pendingValid_[at] = true; }
        else values_[at].store(value.normalized);
        if (echo) {
            if (delayedEcho) enqueueEcho(index, value.normalized);
            else broadcastParam(index, ParamEventKind::Value, value.normalized);
        }
        return true;
    }
    bool hasPendingParamChanges() const noexcept override { return pendingValid_[0] || pendingValid_[1]; }
    std::vector<std::string> stateRoles() const override { return {"chunk"}; }
    std::vector<std::uint8_t> saveState(const std::string& role) const override {
        return role == "chunk" ? patchBytes(patch()) : std::vector<std::uint8_t>{};
    }
    bool loadState(const std::string& role, const std::vector<std::uint8_t>& bytes) override {
        probe_->ownerCallOnAudio = probe_->ownerCallOnAudio || audio::onAudioThread;
        const auto decoded = readPatch(bytes);
        const bool accepted = role == "chunk" && decoded && !refuseStates;
        probe_->add({CallKind::State, -1, decoded.value_or(Patch{}), accepted});
        if (!accepted) return false;
        ++probe_->loads;
        setPatch(*decoded); pendingValid_.fill(false);
        return true;
    }
    std::uint64_t stateEpoch() const noexcept override {
        if (epochReadsUntilGesture_ != 0 && --epochReadsUntilGesture_ == 0) {
            // Deterministic owner-side interleaving: publish a completed input
            // inside an owner capture pass's epoch read, before its ring drain.
            auto* self = const_cast<RuntimeDevice*>(this);
            self->broadcastParam(0, ParamEventKind::Begin, 0);
            self->values_[0].store(injectedGain_);
            self->broadcastParam(0, ParamEventKind::Value, injectedGain_);
            self->broadcastParam(0, ParamEventKind::End, 0);
            ++epochGestureInjections_;
        }
        return epoch_;
    }
    void injectGestureOnEpochRead(unsigned reads, double gain) {
        require(reads != 0 && std::isfinite(gain) && gain >= 0 && gain <= 1, "valid deterministic epoch hook");
        epochReadsUntilGesture_ = reads; injectedGain_ = gain;
    }
    unsigned epochGestureInjections() const noexcept { return epochGestureInjections_; }
    void process(const NodeIo& io) noexcept override {
        for (std::size_t i = 0; i < pendingValid_.size(); ++i) {
            if (pendingValid_[i]) { values_[i].store(pending_[i]); pendingValid_[i] = false; }
        }
        const double gain = values_[0].load(), bias = values_[1].load();
        if (io.wide()) ++probe_->doubleCalls; else ++probe_->floatCalls;
        for (std::int32_t channel = 0; channel < io.channels; ++channel) {
            for (std::int32_t frame = 0; frame < io.frames; ++frame) {
                const auto at = io.blockOffset + frame;
                if (io.wide()) {
                    const auto input = io.in64 && io.in64[channel] ? io.in64[channel][at] : 0.0;
                    io.out64[channel][at] = input * gain + bias;
                } else {
                    const auto input = io.in && io.in[channel] ? io.in[channel][at] : 0.0f;
                    io.out[channel][at] = input * static_cast<float>(gain) + static_cast<float>(bias);
                }
            }
        }
    }
    Patch patch() const noexcept { return {values_[0].load(), values_[1].load(), patch_}; }
    void ownGesture(int index, double value) {
        require(index >= 0 && index < 2, "fake gesture index");
        broadcastParam(index, ParamEventKind::Begin, 0);
        values_[static_cast<std::size_t>(index)].store(value);
        broadcastParam(index, ParamEventKind::Value, value);
        broadcastParam(index, ParamEventKind::End, 0);
    }
    void preset(Patch value, bool broadcasts = true) {
        const auto signal = beginStateSignal();
        publishPresetInsideSignal(value, broadcasts);
    }
    // The test owns a real public StateSignal across an explicit thread
    // barrier. This supplies only fake SDK data, never a production hook.
    void publishPresetInsideSignal(Patch value, bool broadcasts = true) {
        setPatch(value); pendingValid_.fill(false); ++epoch_;
        if (broadcasts) {
            broadcastParam(0, ParamEventKind::Value, value.gain);
            broadcastParam(1, ParamEventKind::Value, value.bias);
        }
    }
    void stateEcho() noexcept { const auto signal = beginStateSignal(); ++epoch_; }
    void silentPatch(Patch value) noexcept { setPatch(value); pendingValid_.fill(false); }
    bool answerOneEcho() {
        if (echoRead_ == echoCount_) return false;
        const auto value = echoes_[echoRead_++];
        broadcastParam(value.first, ParamEventKind::Value, value.second);
        return true;
    }
    bool echo = false, delayedEcho = false, deferred = false;
    bool refuseParameters = false, refuseStates = false;
    int refuseParameter = -1;
    std::optional<std::uint64_t> refuseParameterInPatch;
private:
    static int indexOf(const std::string& id) noexcept { return id == "gain" ? 0 : id == "bias" ? 1 : -1; }
    void setPatch(Patch value) noexcept { values_[0].store(value.gain); values_[1].store(value.bias); patch_ = value.number; }
    void enqueueEcho(int index, double value) noexcept {
        if (echoCount_ < echoes_.size()) echoes_[echoCount_++] = {index, value};
        else probe_->overflow = true;
    }
    std::shared_ptr<Probe> probe_;
    device::DeviceIdentity identity_;
    std::array<device::ParamDescriptor, 2> descriptors_;
    std::array<std::atomic<double>, 2> values_{{0.5, 0}};
    std::array<double, 2> pending_{};
    std::array<bool, 2> pendingValid_{};
    std::array<std::pair<int, double>, 256> echoes_{};
    std::size_t echoRead_ = 0, echoCount_ = 0;
    std::uint64_t patch_ = 0, epoch_ = 0;
    mutable unsigned epochReadsUntilGesture_ = 0, epochGestureInjections_ = 0;
    double injectedGain_ = 0;
};
class ConstantSource final : public Node {
public:
    bool processesDouble() const noexcept override { return true; }
    const char* name() const noexcept override { return "runtime test source"; }
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t channel = 0; channel < io.channels; ++channel) {
            if (io.wide()) std::fill_n(io.out64[channel] + io.blockOffset, io.frames, 0.5);
            else std::fill_n(io.out[channel] + io.blockOffset, io.frames, 0.5f);
        }
    }
};

void seedProject(Store& store, bool devicePresent = true) {
    store.db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Probe',0),(9,'master','Main',1);"
                    "INSERT INTO mixer_strip(track_id) VALUES(1),(9);"
                    "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) VALUES(1,'track',1,'track',9,'main');"
                    "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.param.live','Runtime probe','effect');"
                    "INSERT INTO device_chains(id,track_id) VALUES(1,1)");
    if (devicePresent) store.db().exec("INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(7,1,0,1,'Runtime probe')");
}
OpRequest parameter(double value, const char* id = "gain", Actor actor = Actor::Agent) {
    OpRequest request; request.opType = "device.setParam"; request.actor = actor;
    request.payload = {{"dev", 7}, {"param", id}, {"norm", value}}; return request;
}
std::string writePatch(Store& store, Patch patch) {
    const auto bytes = patchBytes(patch);
    const auto hash = media::blake3Bytes(std::as_bytes(std::span(bytes))).hex;
    require(!hash.empty(), "fixture state hash");
    SQLite::Statement statement(store.db(), "INSERT OR IGNORE INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
    statement.bind(1, hash); statement.bind(2, bytes.data(), static_cast<int>(bytes.size()));
    statement.bind(3, static_cast<std::int64_t>(bytes.size())); statement.exec(); return hash;
}
OpRequest state(Store& store, Patch patch) {
    OpRequest request; request.opType = "device.loadState";
    request.payload = {{"dev", 7}, {"role", "chunk"}, {"hash", writePatch(store, patch)}, {"hint", "vst3"}};
    return request;
}
std::int64_t parameterRows(Store& store) { return store.db().execAndGet("SELECT count(*) FROM plugin_params WHERE device_id=7").getInt64(); }
std::int64_t stateRows(Store& store) { return store.db().execAndGet("SELECT count(*) FROM plugin_state WHERE device_id=7").getInt64(); }
double storedGain(Store& store) {
    return store.db().execAndGet("SELECT normalized_value FROM plugin_params WHERE device_id=7 AND param_id='gain'").getDouble();
}

void render(BlockProcessor& processor, Precision precision, bool irregular, double expected,
            std::int64_t& clock, const std::string& label) {
    std::array<float, 64> left{}, right{};
    std::array<double, 64> left64{}, right64{};
    float* outputs[]{left.data(), right.data()};
    double* outputs64[]{left64.data(), right64.data()};
    constexpr std::array<int, 4> parts{1, 7, 17, 39};
    bool exact = true;
    const auto allocations = callbackAllocations.load();
    const auto fileIo = audio::callbackFileIo.load();
    const std::size_t blocks = irregular ? parts.size() : 1;
    for (std::size_t i = 0; i < blocks; ++i) {
        const auto frames = irregular ? parts[i] : 64;
        left.fill(-7); right.fill(-7); left64.fill(-7); right64.fill(-7);
        AudioIo io; io.out = outputs; io.numOut = 2; io.frames = frames; io.streamTimeSamples = clock;
        if (precision == Precision::Float64) io.out64 = outputs64;
        processor.process(io); clock += frames;
        for (int sample = 0; sample < frames; ++sample) {
            const auto at = static_cast<std::size_t>(sample);
            exact = exact && (precision == Precision::Float64
                ? left64[at] == expected && right64[at] == expected
                : left[at] == static_cast<float>(expected) && right[at] == static_cast<float>(expected));
        }
    }
    check(exact, label + ": every native PCM frame matches the independent literal value");
    check(callbackAllocations.load() == allocations && audio::callbackFileIo.load() == fileIo && !audio::onAudioThread,
          label + ": callback has zero allocation/file I/O and restores thread scope");
}
struct Fixture {
    test::TempDirectory directory{"param_live_replay", "session"};
    std::unique_ptr<Store> store;
    ConstantSource source;
    std::shared_ptr<Probe> probe = std::make_shared<Probe>();
    unsigned loaderCalls = 0;
    RuntimeDevice* device = nullptr;
    Session session;
    ParamOps capture;
    Precision precision;
    bool irregular;
    std::int64_t clock = 0, eventClock = nowMs();
    explicit Fixture(Precision width = Precision::Float32, bool split = false, bool present = true)
        : precision(width), irregular(split) {
        StoreError error{}; store = Store::createEmpty(directory.path() / "song.adi", error);
        require(static_cast<bool>(store), "create native replay fixture"); seedProject(*store, present);
        session.graph().setFadeFrames(0);
        session.setSourcesFor([this](std::int64_t id) { return id == 1 ? std::vector<Node*>{&source} : std::vector<Node*>{}; });
        SessionSpec spec; spec.channels = 2; spec.sampleRate = 48000; spec.maxFrames = 64; spec.precision = precision;
        require(session.load(*store, loader(), spec), "load native replay fixture: " + session.error());
        capture.attachSession(session); session.transport().play();
        if (present) require(device != nullptr, "loaded native device");
    }
    DeviceLoader loader() {
        return [this](const DeviceRequest& request, std::string& error) -> std::unique_ptr<device::DeviceInstance> {
            ++loaderCalls;
            if (!request.ref || request.ref->uid != "test.param.live") { error = "fixture unavailable"; return {}; }
            probe->serial = loaderCalls;
            auto instance = std::make_unique<RuntimeDevice>(probe); device = instance.get(); return instance;
        };
    }
    void sound(double expected, const std::string& name) { render(session, precision, irregular, expected, clock, name); }
    std::int64_t ownerTime() noexcept { return ++eventClock; }
    void ownerDrain(const std::string& label) {
        std::vector<OpRequest> echoes; capture.drain(ownerTime(), echoes);
        check(echoes.empty(), label + ": automatic delivery produces no new parameter gesture");
        check(capture.appliedDeliveryStatus().state == AppliedDeliveryState::Idle,
              label + ": normal owner drain fully acknowledges committed runtime delivery");
    }
    void settleEchoes(const std::string& label) {
        while (device->answerOneEcho()) ownerDrain(label);
        eventClock += 200;
        ownerDrain(label + " after the gesture quiet interval");
    }
    bool commit(const std::vector<OpRequest>& requests, const std::string& name) {
        const auto result = OpJournal(*store).commit(requests);
        check(result.ok, name + ": journal commit succeeds: " + result.error);
        if (result.ok) ownerDrain(name);
        return result.ok;
    }
    bool commit(const OpRequest& request, const std::string& name) { return commit(std::vector<OpRequest>{request}, name); }
    bool undo(const std::string& name) {
        const auto result = History(*store).undo(); check(result.ok, name + ": SQL Undo succeeds: " + result.error);
        if (result.ok) ownerDrain(name);
        return result.ok;
    }
    bool redo(const std::string& name) {
        const auto result = History(*store).redo(); check(result.ok, name + ": SQL Redo succeeds: " + result.error);
        if (result.ok) ownerDrain(name);
        return result.ok;
    }
    std::vector<OpRequest> gesture(double value) {
        std::vector<OpRequest> requests; const auto time = ownerTime();
        eventClock += 3;
        const auto before = device->getParam("gain").normalized;
        require(capture.uiEvent(7, "gain", ParamEventKind::Begin, before, time, requests), "begin fixture gesture");
        require(capture.uiEvent(7, "gain", ParamEventKind::Value, value, time + 1, requests), "move fixture gesture");
        require(capture.uiEvent(7, "gain", ParamEventKind::End, value, time + 2, requests), "end fixture gesture");
        return requests;
    }
    void cancelCaptured(const std::string& label) {
        const auto pending = capture.pendingGestures();
        check(!pending.empty(), label + ": common captured owner remains discoverable");
        for (const auto& item : pending) {
            if (item.state == adi::gesture::State::Committed || item.state == adi::gesture::State::Cancelled) continue;
            require(static_cast<bool>(item.owner), label + ": retained owner token");
            const auto cancelled = capture.cancelPending(*item.owner, ownerTime());
            check(cancelled.status.ok() && cancelled.presentation.state == adi::gesture::PresentationState::Current,
                  label + ": explicit cancel restores and releases ownership");
        }
        capture.pumpGestures(ownerTime());
    }
    bool commitPreset(Patch patch, const std::string& name) {
        device->preset(patch);
        std::vector<OpRequest> requests; capture.drain(ownerTime(), requests);
        std::string error;
        check(!requests.empty() && capture.writeBlobs(*store, error), name + ": producer owns state requests and blobs");
        if (requests.empty() || !error.empty()) return false;
        return commit(requests, name);
    }
    void sameInstance(RuntimeDevice* original, const std::string& name) {
        check(session.instanceFor(7) == original && loaderCalls == 1 && probe->destroyed == 0 &&
                  !probe->overflow && !probe->ownerCallOnAudio,
              name + ": retained instance, one loader call and owner-only delivery");
        check(precision == Precision::Float64 ? probe->doubleCalls > 0 && probe->floatCalls == 0
                                            : probe->floatCalls > 0 && probe->doubleCalls == 0,
              name + ": fake consumed the requested native processing width");
    }
};

void openerAndPresence() {
    for (auto precision : {Precision::Float32, Precision::Float64}) {
        for (bool irregular : {false, true}) {
            Fixture f(precision, irregular); auto* original = f.device; f.device->echo = true;
            f.sound(0.25, "initial unrecorded parameter");
            const auto first = f.gesture(0.75);
            check(first.size() == 2 && first.front().payload.at("norm") == 0.5 && first.back().payload.at("norm") == 0.75,
                  "first gesture carries its real starting-value opener");
            if (!f.commit(first, "first gesture")) continue;
            f.sound(0.375, "committed first gesture");
            if (!f.undo("first gesture Undo")) continue;
            f.sound(0.25, "Undo reaches the pre-gesture instance value");
            check(parameterRows(*f.store) == 0, "Undo clears the first parameter row");
            if (!f.redo("first gesture Redo")) continue;
            f.sound(0.375, "Redo reaches the recorded gesture value");
            if (!f.undo("second Undo")) continue;
            f.sound(0.25, "second Undo restores the same audible origin");
            const auto again = f.gesture(0.625);
            check(again.size() == 2 && again.front().payload.at("norm") == 0.5,
                  "cleared opener bookkeeping makes the next gesture emit a fresh origin");
            if (!f.commit(again, "new gesture after Undo")) continue;
            f.sound(0.3125, "new gesture after Undo");
            f.sameInstance(original, "opener Undo/Redo/Undo");
        }
    }
    Fixture equal; auto* original = equal.device;
    const auto sets = equal.probe->sets;
    if (!equal.commit(parameter(0.5), "equal-value agent row")) return;
    check(parameterRows(*equal.store) == 1 && equal.probe->sets == sets,
          "equal runtime value acknowledges the new row without a redundant setter");
    const auto one = equal.gesture(0.75);
    check(one.size() == 1, "equal-value committed row suppresses a false first-touch opener");
    if (!equal.commit(one, "gesture after equal row") || !equal.undo("undo equal-row gesture")) return;
    equal.sound(0.25, "existing equal row remains a real Undo value");
    check(parameterRows(*equal.store) == 1, "Undo retains the earlier equal row");
    equal.sameInstance(original, "equal row presence");
}

void agentAndPluginEditor() {
    Fixture f; auto* original = f.device; f.device->echo = true;
    if (!f.commit(parameter(0.5), "record existing baseline row")) return;
    if (!f.commit(parameter(0.25), "agent parameter edit")) return;
    f.sound(0.125, "agent edit reaches actual retained instance");
    if (!f.undo("agent Undo")) return;
    f.sound(0.25, "agent Undo");
    if (!f.redo("agent Redo")) return;
    f.sound(0.125, "agent Redo");
    f.device->ownGesture(0, 0.875);
    std::vector<OpRequest> requests; f.capture.drain(f.ownerTime(), requests);
    check(requests.size() == 1 && requests.front().payload.at("norm") == 0.875,
          "plug-in editor emits one edit after an agent-created row");
    if (!f.commit(requests, "plug-in editor gesture")) return;
    f.sound(0.4375, "plug-in editor commit");
    if (!f.undo("plug-in editor Undo")) return;
    f.sound(0.125, "plug-in editor Undo");
    if (!f.redo("plug-in editor Redo")) return;
    f.sound(0.4375, "plug-in editor Redo");
    const auto count = OpJournal(*f.store).count(); f.ownerDrain("post-replay echo drain");
    check(OpJournal(*f.store).count() == count, "replay echoes add no journal entry");
    f.sameInstance(original, "agent and plug-in editor");
}

void stateAndParameterProjection() {
    for (auto precision : {Precision::Float32, Precision::Float64}) {
        Fixture f(precision, true); auto* original = f.device;
        if (!f.commitPreset({0.75, 0.0625, 1}, "preset A")) continue;
        f.sound(0.4375, "preset A actual state");
        const auto gain = f.gesture(0.25);
        if (!f.commit(gain, "parameter newer than preset A")) continue;
        f.sound(0.1875, "newer parameter overlays preset A");
        if (!f.commitPreset({0.5, 0.125, 2}, "preset B and row-follow")) continue;
        f.sound(0.375, "preset B actual state");
        const auto beforeUndoCalls = f.probe->count;
        if (!f.undo("preset B Undo")) continue;
        f.sound(0.1875, "Undo reconciles the captured newer parameter after loading the older chunk");
        check(f.device->patch() == Patch{0.25, 0.0625, 1}, "preset Undo restores patch identity and actual pre-preset parameters");
        bool oldParameter = false, oldChunk = false, finalProjection = false;
        for (std::size_t i = beforeUndoCalls; i < f.probe->count; ++i) {
            const auto& call = f.probe->calls.at(i);
            if (!call.accepted) continue;
            if (call.kind == CallKind::Parameter && call.parameter == 0 && call.value.gain == 0.25) {
                if (oldChunk) finalProjection = true; else oldParameter = true;
            }
            if (call.kind == CallKind::State && call.value == Patch{0.75, 0.0625, 1}) oldChunk = oldParameter;
        }
        check(oldParameter && oldChunk && finalProjection,
              "literal parameter inverse precedes state inverse, then captured row reconciliation");
        if (!f.redo("preset B Redo")) continue;
        f.sound(0.375, "preset B Redo");
        if (!f.undo("preset B Undo again") || !f.undo("parameter Undo")) continue;
        f.sound(0.4375, "Undo parameter returns to preset A's own gain");
        check(parameterRows(*f.store) == 0, "parameter opener is cleared after its value restoration");
        if (!f.undo("preset A Undo")) continue;
        f.sound(0.25, "Undo first preset returns to attach-time sound");
        check(stateRows(*f.store) == 0, "first state opener clear restores absent state role");
        f.device->preset({0.625, 0, 3});
        std::vector<OpRequest> again; f.capture.drain(f.ownerTime(), again);
        std::size_t states = 0;
        for (const auto& request : again) {
            if (request.opType == "device.loadState") ++states;
        }
        check(states == 2, "new preset after Undo captures its state opener again");
        f.sameInstance(original, "state and final parameter projection");
    }
}

void deferredAndEchoes() {
    Fixture f; auto* original = f.device;
    f.device->deferred = true; f.device->echo = true; f.device->delayedEcho = true;
    const auto gesture = f.gesture(0.75);
    check(f.device->getParam("gain").normalized == 0.5 && f.device->hasPendingParamChanges(),
          "deferred backend getter still reports the old audible value");
    if (!f.commit(gesture, "deferred gesture") || !f.undo("deferred Undo before processing")) return;
    f.sound(0.25, "Undo supersedes accepted pending intent before the next callback");
    const auto count = OpJournal(*f.store).count();
    f.settleEchoes("delayed accepted echoes");
    check(OpJournal(*f.store).count() == count, "delayed replay echoes never become another undo step");
    if (!f.redo("deferred Redo")) return;
    f.sound(0.375, "deferred Redo is consumed by native processing");
    f.settleEchoes("delayed Redo echo");
    const auto pending = f.gesture(0.875);
    if (!f.commit(pending, "pending newer parameter")) return;
    if (!f.commit(state(*f.store, {0.625, 0.0625, 8}), "state supersedes queued parameter")) return;
    // The current stored parameter row is .875 and final state reconciliation
    // must follow the committed projection, rather than resurrect an old queue.
    f.sound(0.5, "state and captured final row supersede the older backend queue");
    f.settleEchoes("late pre-state echo");
    f.device->stateEcho(); f.ownerDrain("late state echo");
    f.sameInstance(original, "deferred parameter and state replay");
}

void rollbackDoesNotDeliver() {
    Fixture f;
    const auto before = digestProject(*f.store).text;
    const auto head = OpJournal(*f.store).headSeq();
    const auto count = OpJournal(*f.store).count();
    const auto calls = f.probe->count;
    CommitOptions options;
    options.beforeCommit = [](SQLite::Database&, std::int64_t, std::string& error) {
        error = "fixture refuses after SQL application"; return false;
    };
    const std::array requests{parameter(0.75), parameter(0.125, "bias")};
    const auto rejected = OpJournal(*f.store).commit(requests, options);
    check(!rejected.ok && digestProject(*f.store).text == before && OpJournal(*f.store).headSeq() == head &&
              OpJournal(*f.store).count() == count, "late SQL refusal rolls back parameters, head and journal");
    f.ownerDrain("after refused transaction");
    check(f.probe->count == calls, "rolled-back handler executions produce no runtime delivery");
    f.sound(0.25, "rollback retains original audible state");
}

void branchesAndSnapshot() {
    Fixture f;
    if (!f.commit(parameter(0.5), "branch baseline") || !f.commit(parameter(0.75), "branch A value")) return;
    const auto headA = OpJournal(*f.store).headSeq();
    const auto snapshot = History(*f.store).takeSnapshot("A", 1000, 0);
    check(snapshot.ok, "take actual history snapshot for runtime revert");
    if (!snapshot.ok || !f.undo("fork origin")) return;
    if (!f.commit(parameter(0.25), "branch B value")) return;
    const auto branchB = OpJournal(*f.store).currentBranchId();
    const auto branches = History(*f.store).branches();
    const auto preserved = std::find_if(branches.begin(), branches.end(), [&](const auto& branch) {
        return !branch.isCurrent && branch.headSeq == headA;
    });
    check(preserved != branches.end(), "new edit preserves the abandoned A head in a distinct non-current branch");
    if (preserved == branches.end()) return;
    const auto branchA = preserved->id;
    check(branchA != branchB, "preserved A and current B identify different branches");
    const auto switched = History(*f.store).switchToBranch(branchA);
    check(switched.ok, "switch to real branch A");
    if (!switched.ok) return;
    f.ownerDrain("branch A delivered inverses and forwards"); f.sound(0.375, "branch A audible head");
    const auto back = History(*f.store).switchToBranch(branchB);
    check(back.ok, "switch back to real branch B");
    if (!back.ok) return;
    f.ownerDrain("branch B delivered inverses and forwards"); f.sound(0.125, "branch B audible head");
    const auto reverted = History(*f.store).revertToSnapshot(snapshot.id, 2000);
    check(reverted.ok, "revert to actual named snapshot");
    if (!reverted.ok) return;
    f.ownerDrain("snapshot revert delivered"); f.sound(0.375, "snapshot revert audible state");
    const auto calls = f.probe->count;
    const auto same = History(*f.store).switchToBranch(OpJournal(*f.store).currentBranchId());
    check(same.ok, "same-head branch metadata operation succeeds");
    f.ownerDrain("same-head metadata switch");
    check(f.probe->count == calls, "empty history execution does not invoke a runtime setter");
}

std::size_t acceptedCalls(const Probe& probe, CallKind kind, double gain,
                          std::size_t first = 0) {
    std::size_t result = 0;
    for (std::size_t i = first; i < probe.count && i < probe.calls.size(); ++i) {
        const auto& call = probe.calls.at(i);
        if (call.accepted && call.kind == kind && call.value.gain == gain) ++result;
    }
    return result;
}

void ownReceiptAcknowledgement() {
    {
        Fixture f;
        const auto requests = f.gesture(0.75);
        const auto sets = f.probe->sets;
        if (!f.commit(requests, "own UI receipt")) return;
        check(f.probe->sets == sets, "own UI commit acknowledges values already heard without replaying its opener");
        f.sound(0.375, "own acknowledged UI value");
        if (!f.undo("own UI Undo")) return;
        check(f.probe->sets > sets, "Undo carries no self-acknowledgement authority and restores runtime");
        f.sound(0.25, "own UI Undo audible origin");
        const auto undoSets = f.probe->sets;
        if (!f.redo("own UI Redo")) return;
        check(f.probe->sets > undoSets, "Redo also performs real runtime delivery");
        f.sound(0.375, "own UI Redo audible value");
    }
    {
        Fixture f;
        f.device->ownGesture(0, 0.75);
        std::vector<OpRequest> requests; f.capture.drain(f.ownerTime(), requests);
        const auto calls = f.probe->count;
        if (!f.commit(requests, "own plug-in editor receipt")) return;
        check(f.probe->count == calls, "plug-in editor's own commit does not resend its origin or accepted final value");
        f.sound(0.375, "own acknowledged editor value");
    }
    {
        Fixture f;
        const auto loads = f.probe->loads;
        if (!f.commitPreset({0.75, 0.0625, 1}, "own preset receipt")) return;
        check(f.probe->loads == loads, "own preset receipt never reloads the old and current chunks already heard");
        f.sound(0.4375, "own acknowledged preset");
        if (!f.undo("own preset Undo")) return;
        check(f.probe->loads > loads, "preset Undo performs the real old-state load");
        f.sound(0.25, "own preset Undo origin");
    }
    {
        Fixture f;
        const auto authentic = f.gesture(0.75);
        require(authentic.size() == 2 && authentic.front().capturedTransaction,
                "session-bound complete gesture carries a typed immutable offer");
        auto changed = authentic;
        changed.back().payload["norm"] = 0.625;
        const auto before = f.probe->sets;
        const auto head = OpJournal(*f.store).headSeq();
        const auto count = OpJournal(*f.store).count();
        const auto altered = OpJournal(*f.store).commit(changed);
        const auto partial = OpJournal(*f.store).commit(authentic.front());
        check(!altered.ok && !partial.ok && OpJournal(*f.store).headSeq() == head &&
                  OpJournal(*f.store).count() == count && parameterRows(*f.store) == 0 && f.probe->sets == before,
              "typed copied payload and partial group both refuse before SQL or runtime mutation");
        check(authentic.front().capturedTransaction->phase() == CapturedSubmissionPhase::Offered,
              "tampered copies cannot poison the authentic offer");
        if (!f.commit(authentic, "authentic offer after tampered attempts")) return;
        check(f.probe->sets == before, "authentic complete offer still acknowledges the already-heard gesture");
        f.sound(0.375, "authentic typed offer controls final sound");
    }
    {
        Fixture f;
        auto raw = f.gesture(0.75);
        require(raw.size() == 2, "raw fallback fixture has complete opener group");
        f.cancelCaptured("release owner before independent raw packet");
        for (auto& request : raw) request.capturedTransaction.reset();
        raw.back().payload["norm"] = 0.625;
        f.device->silentPatch({0.25, 0, 0});
        const auto before = f.probe->sets;
        if (!f.commit(raw, "raw changed packet without typed authority")) return;
        check(f.probe->sets >= before + 2, "raw packet without typed authority applies its literal opener and changed final value");
        f.sound(0.3125, "raw changed packet executes literal final value");
    }
    {
        Fixture f;
        auto raw = f.gesture(0.75);
        require(raw.size() == 2, "raw partial fixture has opener group");
        f.cancelCaptured("release owner before independent partial packet");
        for (auto& request : raw) request.capturedTransaction.reset();
        f.device->silentPatch({0.25, 0, 0});
        const auto before = f.probe->sets;
        if (!f.commit(raw.front(), "raw opener only")) return;
        check(f.probe->sets > before, "raw partial group has no self-acknowledgement authority");
        f.sound(0.25, "raw partial packet executes sole opener literally");
    }
    {
        Fixture f;
        const auto requests = f.gesture(0.75);
        f.device->silentPatch({0.25, 0, 0});
        const auto before = f.probe->sets;
        if (!f.commit(requests, "silent runtime mismatch receipt")) return;
        check(f.probe->sets >= before + 2, "unreported runtime mismatch invalidates a previously emitted proof");
        f.sound(0.375, "literal replay repairs silent runtime mismatch");
    }
    {
        Fixture f;
        if (!f.commit(parameter(0.5), "stale receipt baseline") ||
            !f.commit(parameter(0.25), "stale receipt older agent edit")) return;
        auto stale = f.gesture(0.75);
        check(!History(*f.store).undo().ok, "completed uncommitted capture fences History before source/head changes");
        f.cancelCaptured("cancel before independent stale raw receipt");
        for (auto& request : stale) request.capturedTransaction.reset();
        if (!f.undo("intervening history after explicit cancel")) return;
        f.sound(0.25, "intervening Undo changes the actual runtime");
        const auto before = f.probe->sets;
        if (!f.commit(stale, "stale prepared receipt after History")) return;
        check(f.probe->sets > before, "an intervening History publication revokes old self-acknowledgement authority");
        f.sound(0.375, "stale proof falls back to the newly committed literal value");
    }
    {
        Fixture f;
        OpRequest rename; rename.opType = "track.rename"; rename.payload = {{"id", 1}, {"name", "Renamed probe"}};
        if (!f.commit(rename, "history-only proof baseline")) return;
        auto stale = f.gesture(0.75);
        require(stale.size() == 2, "raw historical proof retains its complete first-touch group");
        check(!History(*f.store).undo().ok, "even unrelated History is fenced by an outstanding captured lease");
        f.cancelCaptured("release common owner before raw historical fallback");
        for (auto& request : stale) request.capturedTransaction.reset();
        f.device->silentPatch({0.75, 0, 0});
        const auto image = f.device->patch();
        const auto calls = f.probe->count;
        const auto sequence = f.session.appliedNotifications().sequence();
        const auto* producer = f.capture.captureStats(7);
        const auto* ui = f.capture.uiCaptureStats(7);
        require(producer && ui, "attached capture statistics for sequence-only proof fixture");
        const auto producerPushed = producer->pushed, uiPushed = ui->pushed;
        if (!f.undo("rename Undo after proof capture") || !f.redo("rename Redo after proof capture")) return;
        producer = f.capture.captureStats(7); ui = f.capture.uiCaptureStats(7);
        check(producer && ui && producer->pushed == producerPushed && ui->pushed == uiPushed &&
                  f.device->patch() == image && f.probe->count == calls && parameterRows(*f.store) == 0 &&
                  f.session.appliedNotifications().sequence() == sequence + 2,
              "real History round trip advances only publication sequence while proof runtime image and producer counts stay fixed");
        f.sound(0.375, "same-image History round trip leaves the captured final value audible");
        if (!f.commit(stale, "same-image stale proof after History round trip")) return;
        std::vector<double> values;
        for (std::size_t i = calls; i < f.probe->count && i < f.probe->calls.size(); ++i) {
            const auto& call = f.probe->calls.at(i);
            if (call.accepted && call.kind == CallKind::Parameter && call.parameter == 0) values.push_back(call.value.gain);
        }
        check(values == std::vector<double>{0.5, 0.75},
              "raw historical packet cannot reuse revoked authority and executes literal opener plus final value");
        f.sound(0.375, "literal stale-proof delivery still converges to the unchanged final sound");
    }
    {
        Fixture f;
        const auto requests = f.gesture(0.75);
        const auto committed = OpJournal(*f.store).commit(requests);
        check(committed.ok, "own receipt waits for owner drain before a newer editor input");
        if (!committed.ok) return;
        f.device->ownGesture(0, 0.875);
        const auto before = f.probe->count;
        std::vector<OpRequest> newer; f.capture.drain(f.ownerTime(), newer);
        check(f.capture.appliedDeliveryStatus().state == AppliedDeliveryState::Idle && f.probe->count == before &&
                  newer.size() == 1 && newer.front().payload.at("norm") == 0.875,
              "own receipt acknowledges bookkeeping without clobbering a newer explicit producer gesture");
        f.sound(0.4375, "newer editor input remains audible while older receipt is acknowledged");
        if (!f.commit(newer, "newer editor input receipt")) return;
        check(f.probe->count == before, "newer captured input also acknowledges without a redundant setter");
    }
    {
        Fixture f;
        f.device->preset({0.75, 0.0625, 1});
        std::vector<OpRequest> first; f.capture.drain(f.ownerTime(), first);
        std::string error;
        require(f.capture.writeBlobs(*f.store, error), "write first own-preset blobs");
        const auto committed = OpJournal(*f.store).commit(first);
        check(committed.ok, "own preset receipt waits before newer state signal");
        if (!committed.ok) return;
        f.device->preset({0.5, 0.125, 2});
        const auto before = f.probe->count;
        std::vector<OpRequest> newer; f.capture.drain(f.ownerTime(), newer);
        std::size_t states = 0;
        for (const auto& request : newer) {
            if (request.opType == "device.loadState") ++states;
        }
        check(f.capture.appliedDeliveryStatus().state == AppliedDeliveryState::Idle && f.probe->count == before && states == 1,
              "older own preset acknowledgement preserves a newer state signal and its single later request");
        f.sound(0.375, "newer producer preset is never overwritten by its predecessor receipt");
        require(f.capture.writeBlobs(*f.store, error), "write newer own-preset blobs");
        if (!f.commit(newer, "newer preset receipt")) return;
        check(f.probe->count == before, "newer own preset acknowledgement performs no state reload");
    }
}

void pendingCaptureBoundary() {
    {
        ParamEditCapture ring(3);
        ring.seed(7, 0, 0.5);
        check(!ring.hasPendingEvents(), "new bounded capture ring reports no published events");
        check(ring.push({7, 0, ParamEventKind::Begin, 0}) && ring.hasPendingEvents(),
              "pending accessor observes the first published event");
        check(ring.push({7, 0, ParamEventKind::Value, 0.75}) &&
                  ring.push({7, 0, ParamEventKind::End, 0}) && ring.hasPendingEvents(),
              "full bounded ring still reports pending events");
        check(!ring.push({7, 0, ParamEventKind::Value, 0.125}) && ring.hasPendingEvents() &&
                  ring.stats().pushed == 3 && ring.stats().dropped == 1,
              "refused full-ring push preserves pending status and accepted publication count");
        std::vector<ParamEdit> edits;
        ring.drain(100, edits);
        check(!ring.hasPendingEvents() && edits.size() == 1 && edits.front().before == 0.5 && edits.front().after == 0.75,
              "consuming the published boundary empties the ring and preserves its gesture");
        edits.clear();
        check(ring.push({7, 0, ParamEventKind::Begin, 0}) &&
                  ring.push({7, 0, ParamEventKind::Value, 0.625}) &&
                  ring.push({7, 0, ParamEventKind::End, 0}) && ring.hasPendingEvents() && ring.stats().pushed == 6,
              "wrapped full ring reports unread events without confusing wrap with emptiness");
        ring.drain(101, edits);
        check(!ring.hasPendingEvents() && edits.size() == 1 && edits.front().before == 0.75 && edits.front().after == 0.625,
              "wrapped consumption returns to empty with the exact second gesture");
    }
    {
        Fixture f;
        f.device->ownGesture(0, 0.75);
        // No state acknowledgement is pending. The first owner capture pass
        // consumes the older gesture; retryApplied starts another capture
        // pass before it may deliver receipts. Inject new public callbacks at
        // that second epoch read, after the first consumer boundary. Both
        // accepted intents must stay owned while only the older is offered.
        f.device->injectGestureOnEpochRead(2, 0.875);
        std::vector<OpRequest> older; f.capture.drain(f.ownerTime(), older);
        const auto* captured = f.capture.captureStats(7);
        check(f.device->epochGestureInjections() == 1 && captured && captured->pushed == 6 &&
                  older.size() == 2 && older.front().payload.at("norm") == 0.5 &&
                  older.back().payload.at("norm") == 0.75,
              "capture-pass boundary hook accepts newer input while only the older gesture is emitted");
        check(f.capture.pendingGestures().size() == 2,
              "input injected between capture passes becomes separately retained ownership before receipt delivery");
        const auto committed = OpJournal(*f.store).commit(older);
        check(committed.ok, "older capture-pass interval commits normally");
        if (!committed.ok) return;
        const auto calls = f.probe->count;
        const auto delivered = f.capture.retryApplied(f.ownerTime());
        const auto* after = f.capture.captureStats(7);
        check(delivered.state == AppliedDeliveryState::Idle && after && after->pushed == 6 && f.probe->count == calls,
              "newer accepted input survives older typed acknowledgement after its callback pushes are already observed");
        f.sound(0.4375, "queued newer gesture stays audible across older self acknowledgement");
        std::vector<OpRequest> newer; f.capture.drain(f.ownerTime(), newer);
        check(newer.size() == 1 && newer.front().payload.at("norm") == 0.875,
              "later drain emits the still-pending newer gesture exactly once");
        if (!f.commit(newer, "later capture-pass gesture")) return;
        check(f.probe->count == calls, "later queued gesture acknowledges without redundant runtime delivery");
        f.sound(0.4375, "later queued gesture commit retains its actual value");
    }
}

void completeCaptureAcknowledgement() {
    {
        Fixture f;
        const auto older = f.gesture(0.75);
        std::vector<OpRequest> newer;
        require(f.capture.uiEvent(7, "gain", ParamEventKind::Begin, 0.75, f.ownerTime(), newer),
                "begin newer UI gesture before older own commit");
        require(f.capture.uiEvent(7, "gain", ParamEventKind::Value, 0.875, f.ownerTime(), newer),
                "accept newer UI value before older own commit");
        bool sameOlderOffer = newer.empty() || newer.size() == older.size();
        for (std::size_t index = 0; index < std::min(newer.size(), older.size()); ++index) {
            const auto& actual = newer[index];
            const auto& expected = older[index];
            sameOlderOffer = sameOlderOffer && actual.capturedTransaction == expected.capturedTransaction &&
                actual.opType == expected.opType && encodePayload(actual.payload) == encodePayload(expected.payload) &&
                actual.targetKind == expected.targetKind && actual.targetId == expected.targetId &&
                actual.label == expected.label && actual.selBefore == expected.selBefore &&
                actual.runtimeProof == expected.runtimeProof && actual.actor == expected.actor &&
                actual.actorDetail == expected.actorDetail;
        }
        const auto openPending = f.capture.pendingGestures();
        check(sameOlderOffer && openPending.size() == 2 && openPending.back().requests.empty(),
              "newer explicit UI input stays owned and unoffered while any repeated output is exactly the older capability");
        const auto before = f.probe->count;
        if (!f.commit(older, "older UI receipt with a newer open UI gesture")) return;
        check(f.probe->count == before,
              "older own acknowledgement observes newer accepted UI input without replaying the old opener");
        f.sound(0.4375, "newer open UI value stays audible through older commit acknowledgement");
        newer.clear(); // The repeated older capability has just committed once.
        const auto stillOpen = f.capture.pendingGestures();
        check(stillOpen.size() == 1 && stillOpen.front().requests.empty() && !History(*f.store).undo().ok,
              "older durable acknowledgement retains the newer unfinished UI owner and its History fence");
        require(f.capture.uiEvent(7, "gain", ParamEventKind::End, 0.875, f.ownerTime(), newer),
                "finish newer UI gesture after older acknowledgement");
        check(newer.size() == 1 && newer.front().payload.at("norm") == 0.875,
              "older acknowledgement preserves the newer UI gesture until its own End");
        if (!f.commit(newer, "newer completed UI receipt")) return;
        check(f.probe->count == before, "newer UI receipt also needs no redundant setter");
        if (!f.undo("newer UI Undo")) return;
        f.sound(0.375, "newer UI Undo returns to the earlier committed gesture");
        if (!f.undo("older UI Undo")) return;
        f.sound(0.25, "older UI Undo still restores the original opener");
    }
    {
        Fixture f;
        f.device->ownGesture(0, 0.75);
        f.device->ownGesture(0, 0.625);
        std::vector<OpRequest> first; f.capture.drain(f.ownerTime(), first);
        check(first.size() == 2 && first.front().payload.at("norm") == 0.5 && first.back().payload.at("norm") == 0.75,
              "one legacy vector offers only the first completed gesture and its opener");
        check(!History(*f.store).undo().ok, "owned FIFO fences History before either captured gesture commits");
        const auto before = f.probe->count;
        const auto committed = OpJournal(*f.store).commit(first);
        check(committed.ok, "first completed editor gesture commits separately");
        if (!committed.ok) return;
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Idle && f.probe->count == before,
              "older complete offer acknowledges without replaying over newer accepted input");
        f.sound(0.3125, "newer editor value survives older gesture commit");
        std::vector<OpRequest> second; f.capture.drain(f.ownerTime(), second);
        check(second.size() == 1 && second.front().payload.at("norm") == 0.625,
              "second completed gesture stays owned and is offered FIFO on the later drain");
        if (!f.commit(second, "second editor gesture")) return;
        check(f.probe->count == before, "separate FIFO receipts avoid redundant intermediate setters");
        if (!f.undo("second gesture Undo")) return;
        f.sound(0.375, "second gesture Undo restores its own preceding value");
        if (!f.undo("first gesture Undo")) return;
        check(parameterRows(*f.store) == 0, "first gesture Undo alone clears the first-touch row");
        f.sound(0.25, "first gesture Undo restores original sound");
        if (!f.redo("first gesture Redo") || !f.redo("second gesture Redo")) return;
        check(f.probe->count > before, "History replays both stored gestures with no private acknowledgement authority");
        f.sound(0.3125, "separate FIFO Redo reaches final editor value");
    }
    {
        Fixture f(Precision::Float64, true);
        f.device->ownGesture(0, 0.75);
        f.device->preset({0.625, 0.125, 4});
        std::vector<OpRequest> first; f.capture.drain(f.ownerTime(), first);
        check(first.size() == 2 && first.front().payload.at("norm") == 0.5 && first.back().payload.at("norm") == 0.75,
              "explicit gesture before preset is offered as its own complete vector");
        const auto before = f.probe->count;
        const auto firstCommit = OpJournal(*f.store).commit(first);
        check(firstCommit.ok, "older scalar before preset commits");
        if (!firstCommit.ok) return;
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Idle && f.probe->count == before,
              "older scalar receipt cannot overwrite the later captured preset");
        f.sound(0.4375, "later preset remains audible through older scalar receipt");
        std::vector<OpRequest> preset; f.capture.drain(f.ownerTime(), preset);
        std::size_t states = 0;
        for (const auto& request : preset) if (request.opType == "device.loadState") ++states;
        check(states == 2 && !preset.empty(), "next vector retains state opener and complete preset packet");
        const auto blobs = f.store->db().execAndGet("SELECT count(*) FROM state_blobs").getInt64();
        const auto changes = sqlite3_total_changes64(f.store->db().getHandle());
        std::string error;
        require(f.capture.writeBlobs(*f.store, error), "stage native preset resources");
        check(f.store->db().execAndGet("SELECT count(*) FROM state_blobs").getInt64() == blobs &&
                  sqlite3_total_changes64(f.store->db().getHandle()) == changes,
              "session-bound writeBlobs stages complete resources with zero SQL writes");
        if (!f.commit(preset, "separate preset gesture")) return;
        check(f.probe->count == before, "preset own receipt adds no state reload or parameter setter");
        if (!f.undo("preset gesture Undo")) return;
        check(stateRows(*f.store) == 0 && parameterRows(*f.store) == 1,
              "preset Undo clears its state opener while preserving preceding scalar transaction");
        f.sound(0.375, "preset Undo restores the preceding committed scalar sound");
        if (!f.undo("preceding scalar Undo")) return;
        check(parameterRows(*f.store) == 0, "earlier scalar Undo clears its own opener");
        f.sound(0.25, "separate mixed gestures restore original sound");
        if (!f.redo("preceding scalar Redo") || !f.redo("preset gesture Redo")) return;
        check(f.probe->count > before, "mixed History performs actual state and parameter delivery");
        f.sound(0.4375, "mixed FIFO Redo restores final preset");
    }
}

void capturedOwnershipAndResources() {
    {
        Fixture f;
        const auto allocations = callbackAllocations.load();
        const auto fileIo = audio::callbackFileIo.load();
        const auto sqlChanges = sqlite3_total_changes64(f.store->db().getHandle());
        {
            audio::CallbackScope callback;
            f.device->preset({0.75, 0.0625, 12});
        }
        check(callbackAllocations.load() == allocations && audio::callbackFileIo.load() == fileIo &&
                  sqlite3_total_changes64(f.store->db().getHandle()) == sqlChanges && !audio::onAudioThread,
              "state and parameter callback reservations allocate nothing, write no SQL and perform no file I/O");
        check(!History(*f.store).undo().ok, "unmuted callback state signal fences History before any owner snapshot");
        std::vector<OpRequest> offered; f.capture.drain(f.ownerTime(), offered);
        check(!offered.empty() && offered.front().capturedTransaction,
              "callback state and parameter watermarks transfer into a complete owned preset");
        f.cancelCaptured("release callback state capture");
        f.sound(0.25, "callback preset cancel restores origin");
    }
    {
        Fixture f;
        if (!f.commit(parameter(0.5), "queued input navigation baseline")) return;
        f.device->ownGesture(0, 0.75);
        const auto head = OpJournal(*f.store).headSeq();
        const auto sequence = appliedSource(*f.store).sequence();
        const auto branch = OpJournal(*f.store).currentBranchId();
        check(!History(*f.store).undo().ok && !History(*f.store).switchToBranch(branch).ok &&
                  !f.store->saveAs(f.directory.path() / "queued.adi").ok &&
                  OpJournal(*f.store).headSeq() == head && appliedSource(*f.store).sequence() == sequence,
              "undrained callback reservation fences navigation including same-head paths and Save As");
        std::vector<OpRequest> offered; f.capture.drain(f.ownerTime(), offered);
        check(!offered.empty() && !History(*f.store).undo().ok,
              "draining transfers callback reservation to a retained source lease without a navigation gap");
        f.cancelCaptured("explicitly cancel drained callback input");
        f.sound(0.25, "cancel restores pre-gesture audible value");
        check(History(*f.store).undo().ok, "completed cancellation releases navigation fence");
        f.ownerDrain("cancelled input followed by real Undo");
    }
    {
        Fixture f(Precision::Float64, true);
        const auto blobs = f.store->db().execAndGet("SELECT count(*) FROM state_blobs").getInt64();
        const auto head = OpJournal(*f.store).headSeq();
        const auto count = OpJournal(*f.store).count();
        f.device->preset({0.75, 0.0625, 9});
        std::vector<OpRequest> offered; f.capture.drain(f.ownerTime(), offered);
        require(!offered.empty() && offered.front().capturedTransaction, "preset exports typed atomic resources");
        const auto changes = sqlite3_total_changes64(f.store->db().getHandle());
        std::string error;
        require(f.capture.writeBlobs(*f.store, error), "legacy bridge stages captured state blobs");
        auto moved = f.capture.takeBlobs(); moved.clear();
        check(sqlite3_total_changes64(f.store->db().getHandle()) == changes &&
                  f.store->db().execAndGet("SELECT count(*) FROM state_blobs").getInt64() == blobs,
              "writeBlobs and takeBlobs leave SQL untouched and cannot drop core-owned resources");
        f.store->db().exec("CREATE TRIGGER reject_captured_state BEFORE INSERT ON plugin_state BEGIN SELECT RAISE(ABORT,'owned preset refusal'); END");
        const auto rejected = OpJournal(*f.store).commit(offered);
        check(!rejected.ok && offered.front().capturedTransaction->phase() == CapturedSubmissionPhase::Refused &&
                  OpJournal(*f.store).headSeq() == head && OpJournal(*f.store).count() == count &&
                  stateRows(*f.store) == 0 && f.store->db().execAndGet("SELECT count(*) FROM state_blobs").getInt64() == blobs,
              "late captured commit refusal rolls resources, material, head and journal back atomically");
        check(!OpJournal(*f.store).commit(offered).ok && !History(*f.store).undo().ok,
              "refused offer remains fenced and cannot be implicitly resubmitted");
        f.sound(0.4375, "refused preset remains heard and owned");
        const auto pending = f.capture.pendingGestures();
        require(pending.size() == 1 && pending.front().owner, "refused preset remains discoverable after caller drops resources");
        f.store->db().exec("DROP TRIGGER reject_captured_state");
        const auto retried = f.capture.retryPendingCommit(*pending.front().owner, f.ownerTime());
        check(retried.status.ok() && retried.durable && retried.durable->ok,
              "explicit retry uses retained complete preset resources");
        const auto after = OpJournal(*f.store).count();
        check(after > count && stateRows(*f.store) == 1 &&
                  f.store->db().execAndGet("SELECT count(*) FROM state_blobs").getInt64() > blobs,
              "resources become durable only with their state transaction");
        check(!f.capture.retryPendingCommit(*pending.front().owner, f.ownerTime()).status.ok() &&
                  OpJournal(*f.store).count() == after, "durable retained owner cannot submit twice");
        f.ownerDrain("explicit preset retry acknowledgement");
        if (!f.undo("retried preset Undo")) return;
        f.sound(0.25, "retried preset has one correct Undo boundary");
    }
    {
        Fixture f;
        f.device->preset({0.75, 0.0625, 10});
        std::vector<OpRequest> offered; f.capture.drain(f.ownerTime(), offered);
        const auto pending = f.capture.pendingGestures();
        require(pending.size() == 1 && pending.front().owner, "state cancel fixture owns one gesture");
        f.device->refuseStates = true;
        const auto cancelled = f.capture.cancelPending(*pending.front().owner, f.ownerTime());
        check(cancelled.presentation.state != adi::gesture::PresentationState::Current &&
                  !History(*f.store).undo().ok && !f.capture.pendingGestures().empty(),
              "SDK-refused restoration retains cancelled intent, owner and navigation lease");
        check(!f.capture.commitPending(*pending.front().owner, f.ownerTime()).status.ok(),
              "sticky Cancelling cannot turn into a commit");
        f.sound(0.4375, "refused restoration leaves actual current sound observable");
        f.device->refuseStates = false;
        const auto restored = f.capture.retryPendingCancel(*pending.front().owner, f.ownerTime());
        check(restored.status.ok() && restored.presentation.state == adi::gesture::PresentationState::Current &&
                  parameterRows(*f.store) == 0 && stateRows(*f.store) == 0,
              "explicit cancel retry restores full origin without a journal transaction");
        f.sound(0.25, "cancel retry restores actual origin sound");
    }
}

void concurrentCaptureBindings() {
    {
        Fixture f;
        std::atomic<bool> entered{false}, release{false};
        std::jthread producer([&](std::stop_token stop) {
            const auto inFlight = f.device->beginStateSignal();
            f.device->publishPresetInsideSignal({0.75, 0.0625, 13});
            entered.store(true, std::memory_order_release);
            // Scheduling barrier is deliberately outside allocation auditing;
            // no production callback waits on the owner or invokes a lock.
            while (!release.load(std::memory_order_acquire) && !stop.stop_requested()) std::this_thread::yield();
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        const bool arrived = entered.load(std::memory_order_acquire);
        check(arrived, "threaded state callback reaches deterministic in-flight publication barrier");
        if (!arrived) return; // jthread requests stop before joining.
        check(!f.device->captureCallbacksQuiescent() && !History(*f.store).undo().ok,
              "announced callback retains its binding and reserves source navigation before finish");
        f.capture.detach(7);
        const auto reboundEarly = f.capture.attachSession(f.session);
        check(reboundEarly == 0 && !f.capture.isAttached(7) && !f.device->collectCaptureBindings(),
              "owner detach unpublishes input and rebind defers without waiting while old callback is announced");
        std::vector<OpRequest> premature; f.capture.drain(f.ownerTime(), premature);
        check(premature.empty() && !History(*f.store).switchToBranch(OpJournal(*f.store).currentBranchId()).ok,
              "unfinished state watermark cannot be snapshotted or lose its navigation reservation during detach");
        release.store(true, std::memory_order_release); producer.join();
        check(f.device->captureCallbacksQuiescent() && f.capture.attachSession(f.session) == 1,
              "owner can rebind after the actual producer announcement finishes");
        std::vector<OpRequest> retained; f.capture.drain(f.ownerTime(), retained);
        check(!retained.empty() && retained.front().capturedTransaction && !f.capture.pendingGestures().empty() &&
                  parameterRows(*f.store) == 0 && stateRows(*f.store) == 0,
              "quiescent rebind preserves old callback input and its original owned cancellation baseline");
        f.sound(0.4375, "retained concurrent preset remains audible before explicit recovery");
        f.cancelCaptured("cancel callback retained through detach/rebind");
        f.sound(0.25, "explicit callback recovery restores original .5 gain and zero bias");
        check(History(*f.store).switchToBranch(OpJournal(*f.store).currentBranchId()).ok,
              "completed recovery releases every old binding reservation and owner lease");
        f.ownerDrain("same-head navigation after callback recovery");
        if (!f.commit(f.gesture(0.625), "new capture after concurrent recovery")) return;
        if (!f.undo("new capture after recovery Undo")) return;
        f.sound(0.25, "new binding has correct origin after old cancellation");
    }
    {
        // Two immutable public bindings may retain one ring through an owner
        // exchange. Writer exclusion belongs to the ring, not either binding.
        constexpr int eventsPerProducer = 6000;
        auto ring = std::make_shared<ParamEditCapture>(eventsPerProducer * 2 + 1);
        for (int index = 0; index < eventsPerProducer * 2; ++index) ring->seed(7, index, 0.0);
        auto oldBinding = std::make_shared<device::DeviceCaptureBinding>();
        auto newBinding = std::make_shared<device::DeviceCaptureBinding>();
        oldBinding->deviceId = 7; oldBinding->parameters = ring;
        newBinding->deviceId = 7; newBinding->parameters = ring;
        auto probe = std::make_shared<Probe>(); RuntimeDevice instance(probe);
        instance.installCaptureBinding(oldBinding);
        std::atomic<unsigned> ready{0}; std::atomic<bool> start{false};
        std::array<unsigned, 2> accepted{};
        std::array<std::vector<std::uint8_t>, 2> acceptedEvents{
            std::vector<std::uint8_t>(eventsPerProducer), std::vector<std::uint8_t>(eventsPerProducer)};
        auto write = [&](unsigned index, const std::shared_ptr<device::DeviceCaptureBinding>& binding, std::stop_token stop) {
            const auto announced = instance.beginStateSignal();
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire) && !stop.stop_requested()) std::this_thread::yield();
            if (stop.stop_requested()) return;
            for (int event = 1; event <= eventsPerProducer; ++event) {
                const auto value = static_cast<double>(event) / eventsPerProducer;
                const auto parameter = static_cast<std::int32_t>(index) * eventsPerProducer + event - 1;
                if (binding->parameters->push({7, parameter, ParamEventKind::Value, value})) {
                    ++accepted[index]; acceptedEvents[index][static_cast<std::size_t>(event - 1)] = 1;
                }
                if ((event % 127) == 0) std::this_thread::yield();
            }
        };
        std::jthread first([&](std::stop_token stop) { write(0, oldBinding, stop); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (ready.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        instance.detachCaptureBinding(); instance.installCaptureBinding(newBinding);
        std::jthread second([&](std::stop_token stop) { write(1, newBinding, stop); });
        while (ready.load(std::memory_order_acquire) != 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        const bool arrived = ready.load(std::memory_order_acquire) == 2;
        check(arrived && !instance.captureCallbacksQuiescent() && !instance.collectCaptureBindings(),
              "old and new binding owners survive an exchange with two actual producer announcements");
        start.store(true, std::memory_order_release); first.join(); second.join();
        const auto stats = ring->stats();
        check(stats.pushed == static_cast<std::int64_t>(accepted[0] + accepted[1]) &&
                  stats.pushed + stats.dropped == eventsPerProducer * 2 && accepted[0] != 0 && accepted[1] != 0,
              "shared ring writer exclusion accounts for every accepted or explicitly refused concurrent publication");
        std::vector<ParamEdit> edits; ring->drain(1000, edits); ring->drain(1200, edits);
        std::vector<bool> seen(static_cast<std::size_t>(eventsPerProducer * 2), false);
        bool exact = edits.size() == accepted[0] + accepted[1];
        for (const auto& edit : edits) {
            if (edit.paramIndex >= 0 && edit.paramIndex < eventsPerProducer * 2) {
                const auto at = static_cast<std::size_t>(edit.paramIndex);
                const auto producerIndex = at / static_cast<std::size_t>(eventsPerProducer);
                const auto eventIndex = at % static_cast<std::size_t>(eventsPerProducer);
                exact = exact && !seen[at] && acceptedEvents[producerIndex][eventIndex] != 0 && edit.before == 0 &&
                    edit.after == static_cast<double>(eventIndex + 1) / eventsPerProducer;
                seen[at] = true;
            } else exact = false;
        }
        check(exact, "binding overlap preserves every accepted unique edit without collided slots, loss or duplication");
        instance.detachCaptureBinding();
        check(instance.captureCallbacksQuiescent() && instance.collectCaptureBindings(),
              "old and new binding owners are reclaimable only after both producer threads finish");
    }
}

void refusedCapturedFifo() {
    for (const bool cancelOlder : {false, true}) {
        Fixture f;
        f.device->ownGesture(0, 0.75);
        f.device->ownGesture(0, 0.625);
        std::vector<OpRequest> first; f.capture.drain(f.ownerTime(), first);
        require(first.size() == 2 && first.front().capturedTransaction, "FIFO refusal has first complete typed offer");
        auto pending = f.capture.pendingGestures();
        require(pending.size() == 2 && pending.front().owner, "both completed same-endpoint gestures are owned");
        const auto firstOwner = pending.front().owner;
        const auto calls = f.probe->count;
        f.store->db().exec("CREATE TRIGGER reject_first_capture BEFORE INSERT ON plugin_params BEGIN SELECT RAISE(ABORT,'first FIFO refusal'); END");
        const auto refused = OpJournal(*f.store).commit(first);
        check(!refused.ok && first.front().capturedTransaction->phase() == CapturedSubmissionPhase::Refused &&
                  parameterRows(*f.store) == 0 && f.capture.pendingGestures().size() == 2,
              "first commit refusal retains both owned gestures and absent committed row");
        check(!History(*f.store).undo().ok && !History(*f.store).switchToBranch(OpJournal(*f.store).currentBranchId()).ok,
              "refused first plus newer owned FIFO blocks all History paths");
        f.sound(0.3125, "newer input remains audible after older refusal");
        std::vector<OpRequest> notReady; f.capture.drain(f.ownerTime(), notReady);
        check(notReady.empty() && f.probe->count == calls, "ordinary tick neither retries refusal nor drops FIFO order");
        f.store->db().exec("DROP TRIGGER reject_first_capture");
        if (cancelOlder) {
            const auto cancelled = f.capture.cancelPending(*firstOwner, f.ownerTime());
            check(cancelled.status.ok() && parameterRows(*f.store) == 0,
                  "explicit older cancel leaves actual committed endpoint absent");
        } else {
            const auto retried = f.capture.retryPendingCommit(*firstOwner, f.ownerTime());
            check(retried.status.ok() && retried.durable && retried.durable->ok && storedGain(*f.store) == 0.75,
                  "explicit older retry commits only its retained value");
            check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Idle,
                  "older retried receipt is acknowledged while newer input remains owned");
        }
        check(f.probe->count == calls && !History(*f.store).undo().ok,
              "older terminal action never restores over newer accepted ordinal and later owner keeps History fenced");
        f.sound(0.3125, "newer input survives older cancel or retry");
        std::vector<OpRequest> second; f.capture.drain(f.ownerTime(), second);
        check(second.size() == (cancelOlder ? 2u : 1u) && !second.empty() &&
                  second.back().payload.at("norm") == 0.625 &&
                  (!cancelOlder || second.front().payload.at("norm") == 0.5),
              "later FIFO reprepare derives opener from actual committed base, never cancelled intermediate input");
        if (!f.commit(second, "later gesture after retained first refusal")) continue;
        check(f.probe->count == calls, "later own acknowledgement adds no redundant SDK setter");
        if (!f.undo("later FIFO Undo")) continue;
        f.sound(cancelOlder ? 0.25 : 0.375, "later Undo restores actual prior committed sound");
        if (cancelOlder) {
            check(parameterRows(*f.store) == 0 && !History(*f.store).canUndo(),
                  "cancel-first creates one undoable gesture and restores row absence");
        } else {
            if (!f.undo("earlier retried FIFO Undo")) continue;
            check(parameterRows(*f.store) == 0, "second Undo separately clears first gesture opener");
            f.sound(0.25, "retry-first preserves two exact Undo boundaries");
        }
    }
}

void refusedParameterAndFifo() {
    Fixture f;
    if (!f.commit(std::vector<OpRequest>{parameter(0.5), parameter(0, "bias")}, "FIFO baseline rows")) return;
    f.device->refuseParameter = 1;
    const auto firstCall = f.probe->count;
    const std::array requests{parameter(0.75), parameter(0.125, "bias"), parameter(0.875)};
    const auto committed = OpJournal(*f.store).commit(requests);
    check(committed.ok, "parameter refusal fixture still commits SQL successfully");
    if (!committed.ok) return;
    check(f.probe->count == firstCall && f.capture.appliedDeliveryStatus().state == AppliedDeliveryState::Pending,
          "Store notification enqueues without invoking any plug-in setter");
    const auto refused = f.capture.retryApplied(f.ownerTime());
    check(refused.state == AppliedDeliveryState::Refused && refused.pendingBatchCount == 1 &&
              refused.opIndex == 1 && !refused.reconcilingParameters && !refused.error.empty(),
          "refused parameter reports the exact remaining operation and a runtime error");
    f.sound(0.375, "only the acknowledged prefix is audible before refusal");
    const auto later = OpJournal(*f.store).commit(parameter(0.25));
    check(later.ok, "later SQL batch may commit while runtime delivery is blocked");
    if (!later.ok) return;
    require(f.session.refresh(*f.store), "refresh unchanged instance while queue is blocked");
    f.capture.attachSession(f.session);
    const auto stillRefused = f.capture.retryApplied(f.ownerTime());
    check(stillRefused.state == AppliedDeliveryState::Refused && stillRefused.pendingBatchCount == 2 &&
              stillRefused.batchSerial == refused.batchSerial && stillRefused.opIndex == refused.opIndex &&
              acceptedCalls(*f.probe, CallKind::Parameter, 0.75, firstCall) == 1,
          "same-source refresh and attachment preserve FIFO, failure cursor and acknowledged prefix");
    f.device->refuseParameter = -1;
    const auto retried = f.capture.retryApplied(f.ownerTime());
    check(retried.state == AppliedDeliveryState::Idle && retried.pendingBatchCount == 0,
          "lifting parameter refusal resumes the remaining suffix and later batch");
    f.sound(0.25, "remaining bias and gain changes precede the later batch");
    std::vector<double> gainCalls;
    for (std::size_t i = firstCall; i < f.probe->count && i < f.probe->calls.size(); ++i) {
        const auto& call = f.probe->calls.at(i);
        if (call.accepted && call.kind == CallKind::Parameter && call.parameter == 0) gainCalls.push_back(call.value.gain);
    }
    check(gainCalls == std::vector<double>{0.75, 0.875, 0.25},
          "retry never repeats an accepted prefix or overtakes later operations");
}

void refusedOpeners() {
    {
        Fixture f;
        if (!f.commit(f.gesture(0.75), "parameter opener refusal setup")) return;
        const auto cleared = f.capture.stats().appliedCleared;
        f.device->refuseParameters = true;
        const auto undo = History(*f.store).undo();
        check(undo.ok && parameterRows(*f.store) == 0, "SQL Undo succeeds and restores row absence before runtime retry");
        if (!undo.ok) return;
        const auto refused = f.capture.retryApplied(f.ownerTime());
        check(refused.state == AppliedDeliveryState::Refused && refused.opIndex == 0 &&
                  f.capture.stats().appliedCleared == cleared,
              "parameter restore refusal holds the following opener clear");
        f.sound(0.375, "refused parameter restore leaves the old audible value");
        f.device->refuseParameters = false;
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Idle,
              "parameter opener pair completes after refusal is lifted");
        f.sound(0.25, "retried parameter restoration reaches the attach value");
        check(f.capture.stats().appliedCleared == cleared + 1 && f.gesture(0.625).size() == 2,
              "the acknowledged clear permits a fresh parameter opener");
    }
    {
        Fixture f;
        if (!f.commitPreset({0.75, 0.0625, 1}, "state opener refusal setup")) return;
        const auto cleared = f.capture.stats().statesCleared;
        f.device->refuseStates = true;
        const auto undo = History(*f.store).undo();
        check(undo.ok && stateRows(*f.store) == 0, "state SQL Undo restores absent role while runtime may refuse");
        if (!undo.ok) return;
        const auto refused = f.capture.retryApplied(f.ownerTime());
        check(refused.state == AppliedDeliveryState::Refused && refused.opIndex == 0 &&
                  f.capture.stats().statesCleared == cleared,
              "state restore refusal holds its following role-opener clear");
        f.sound(0.4375, "refused state restore retains previous audible patch");
        f.device->refuseStates = false;
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Idle,
              "state opener pair completes after refusal is lifted");
        f.sound(0.25, "retried state restore reaches the actual attach chunk");
        f.device->preset({0.625, 0, 3});
        std::vector<OpRequest> requests; f.capture.drain(f.ownerTime(), requests);
        std::size_t stateCount = 0;
        for (const auto& request : requests) {
            if (request.opType == "device.loadState") ++stateCount;
        }
        check(f.capture.stats().statesCleared == cleared + 1 && stateCount == 2,
              "successful state clear permits a new attach-origin opener");
    }
}

void refusedFinalProjection() {
    Fixture f(Precision::Float64, true);
    if (!f.commitPreset({0.75, 0.0625, 1}, "projection retry preset A") ||
        !f.commit(f.gesture(0.25), "projection retry newer row") ||
        !f.commitPreset({0.5, 0.125, 2}, "projection retry preset B")) return;
    const auto firstCall = f.probe->count;
    f.device->refuseParameterInPatch = 1;
    const auto undo = History(*f.store).undo();
    check(undo.ok, "projection retry actual Undo commits");
    if (!undo.ok) return;
    const auto refused = f.capture.retryApplied(f.ownerTime());
    check(refused.state == AppliedDeliveryState::Refused && refused.reconcilingParameters &&
              refused.projectionIndex == 0 && refused.parameterIndex == 0 && !refused.error.empty(),
          "final-row reconciliation has its own visible refusal cursor");
    f.sound(0.4375, "refused final projection leaves the successfully restored old chunk audible");
    check(acceptedCalls(*f.probe, CallKind::State, 0.75, firstCall) == 1 &&
              acceptedCalls(*f.probe, CallKind::Parameter, 0.25, firstCall) == 1,
          "literal inverse parameter and state each completed before projection refusal");
    const auto later = OpJournal(*f.store).commit(parameter(0.875));
    check(later.ok, "newer SQL row commits behind a refused captured projection");
    if (!later.ok) return;
    require(f.session.refresh(*f.store), "refresh before final-projection retry");
    f.capture.attachSession(f.session);
    const auto unchanged = f.capture.retryApplied(f.ownerTime());
    check(unchanged.state == AppliedDeliveryState::Refused && unchanged.pendingBatchCount == 2 &&
              unchanged.batchSerial == refused.batchSerial && unchanged.reconcilingParameters &&
              unchanged.parameterIndex == refused.parameterIndex,
          "normal repeated attach preserves the final-projection cursor");
    f.device->refuseParameterInPatch.reset();
    const auto retry = f.capture.retryApplied(f.ownerTime());
    check(retry.state == AppliedDeliveryState::Idle, "projection retry advances through later batches");
    f.sound(0.5, "captured projection is followed by the later committed parameter");
    std::vector<double> accepted;
    for (std::size_t i = firstCall; i < f.probe->count && i < f.probe->calls.size(); ++i) {
        const auto& call = f.probe->calls.at(i);
        if (call.accepted && call.kind == CallKind::Parameter && call.parameter == 0) accepted.push_back(call.value.gain);
    }
    check(accepted == std::vector<double>{0.25, 0.25, 0.875} &&
              acceptedCalls(*f.probe, CallKind::State, 0.75, firstCall) == 1,
          "retry uses the pinned .25 row before newer SQL .875 and never repeats the state prefix");
}

void clearOnlyInverseBoundary() {
    Fixture f;
    if (!f.commit(parameter(0.75), "raw agent first-ever row")) return;
    f.sound(0.375, "first raw agent write reaches the instance");
    if (!f.undo("raw first-row clear inverse")) return;
    check(parameterRows(*f.store) == 0, "raw first-row inverse has only row absence to restore");
    // This journal inverse contains no pre-edit audible value. A delivery
    // bridge must not fabricate one; user gestures carry an explicit opener.
    f.sound(0.375, "clear-only inverse does not invent an audible parameter default");
    const auto next = f.gesture(0.625);
    check(next.size() == 2 && next.front().payload.at("norm") == 0.75,
          "cleared raw row seeds the next real gesture from the actual retained value");
}

OpRequest insertDevice() {
    OpRequest request; request.opType = "device.insert";
    request.payload = {{"id", 7}, {"chain", 1}, {"ord", 0}, {"ref", 1}, {"name", "Runtime probe"}};
    return request;
}
OpRequest removeDevice() {
    OpRequest request; request.opType = "device.remove"; request.payload = {{"id", 7}}; return request;
}
void attachmentAndRetirement() {
    {
        Fixture f(Precision::Float32, true, false);
        const std::array requests{insertDevice(), parameter(0.75)};
        const auto commit = OpJournal(*f.store).commit(requests);
        check(commit.ok, "new target insert and parameter commit before Session refresh");
        if (!commit.ok) return;
        const auto waiting = f.capture.retryApplied(f.ownerTime());
        check(waiting.state == AppliedDeliveryState::AwaitingAttachment && waiting.pendingBatchCount == 1 &&
                  f.loaderCalls == 0, "unrealised existing target waits without constructing a plug-in in delivery");
        require(f.session.refresh(*f.store), "refresh newly inserted runtime target");
        f.capture.attachSession(f.session);
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Idle && f.loaderCalls == 1,
              "normal refresh and attach resolve the committed target once");
        f.sound(0.375, "newly attached instance consumes the committed parameter value");
    }
    {
        Fixture f;
        f.capture.detach(7);
        const auto commit = OpJournal(*f.store).commit(parameter(0.75));
        check(commit.ok, "temporarily detached target accepts SQL edit");
        if (!commit.ok) return;
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::AwaitingAttachment,
              "detached current instance waits instead of losing the committed value");
        require(f.session.refresh(*f.store), "refresh detached target at its committed sequence");
        f.capture.attachSession(f.session);
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Idle,
              "reattachment resumes the same target receipt");
        f.sound(0.375, "reattached instance receives its waiting parameter");
    }
    {
        Fixture f;
        const auto calls = f.probe->count;
        const std::array requests{parameter(0.75), removeDevice()};
        const auto commit = OpJournal(*f.store).commit(requests);
        check(commit.ok, "parameter followed by same-batch cascade deletion commits");
        if (!commit.ok) return;
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Idle && f.probe->count == calls,
              "captured final target absence retires delivery before invoking the old instance");
        require(f.session.refresh(*f.store), "refresh deleted device"); f.capture.attachSession(f.session);
        check(!f.capture.isAttached(7), "normal refresh detaches the retired device sink");
        f.sound(0.5, "removed effect leaves the literal source PCM");
    }
    {
        Fixture f;
        f.device->refuseParameters = true;
        const auto edit = OpJournal(*f.store).commit(parameter(0.75));
        check(edit.ok, "older refused target edit commits");
        if (!edit.ok) return;
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Refused,
              "older receipt has a known instance anchor before retirement");
        const auto removed = OpJournal(*f.store).commit(removeDevice());
        check(removed.ok, "later removal commits behind refused runtime work");
        if (!removed.ok) return;
        const auto calls = f.probe->count;
        require(f.session.refresh(*f.store), "refresh target removed behind refusal"); f.capture.attachSession(f.session);
        const auto retired = f.capture.retryApplied(f.ownerTime());
        check(retired.state == AppliedDeliveryState::Idle && retired.pendingBatchCount == 0 && f.probe->count == calls,
              "known retired target acknowledges old work without retrying its obsolete instance: state=" +
                  std::to_string(static_cast<int>(retired.state)) + ", pending=" +
                  std::to_string(retired.pendingBatchCount) + ", error=" + retired.error);
        f.sound(0.5, "retired old receipt cannot alter the current graph");
    }
    {
        Fixture f;
        const auto commit = OpJournal(*f.store).commit(parameter(0.75, "absent.parameter"));
        check(commit.ok, "opaque unknown parameter is valid durable SQL data");
        if (!commit.ok) return;
        const auto refused = f.capture.retryApplied(f.ownerTime());
        check(refused.state == AppliedDeliveryState::Refused && !refused.error.empty() && refused.pendingBatchCount == 1,
              "attached plug-in's unknown parameter is a structured retryable refusal");
        f.sound(0.25, "unknown parameter leaves all known audible values intact");
    }
}

void pinnedStateBytes() {
    Fixture f;
    f.device->refuseStates = true;
    const auto first = state(*f.store, {0.75, 0.0625, 11});
    const auto committed = OpJournal(*f.store).commit(first);
    check(committed.ok, "owned state receipt commits before plug-in refusal");
    if (!committed.ok) return;
    check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Refused,
          "first owned state receipt waits at actual loader refusal");
    const auto second = state(*f.store, {0.625, 0, 12});
    const auto later = OpJournal(*f.store).commit(second);
    check(later.ok, "later state replaces the current SQL state role while an earlier receipt waits");
    if (!later.ok) return;
    // Fault fixture: remove the now-unreferenced old resource after publication.
    // The pending runtime receipt owns the bytes; retry must not reread SQLite.
    SQLite::Statement remove(f.store->db(), "DELETE FROM state_blobs WHERE hash_blake3=?");
    remove.bind(1, first.payload.at("hash").get<std::string>()); remove.exec();
    f.device->refuseStates = false;
    const auto before = f.probe->count;
    check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Idle,
          "delayed state retry uses its owned bytes even after old SQL resource removal");
    std::vector<std::uint64_t> patches;
    for (std::size_t i = before; i < f.probe->count && i < f.probe->calls.size(); ++i) {
        const auto& call = f.probe->calls.at(i);
        if (call.accepted && call.kind == CallKind::State) patches.push_back(call.value.number);
    }
    check(patches == std::vector<std::uint64_t>{11, 12},
          "owned old state and newer state reach the real loader in original receipt order");
    f.sound(0.3125, "newest completed owned state determines final PCM");
}

void blockedCapture(Fixture& f, const std::string& label) {
    const auto calls = f.probe->count;
    std::vector<OpRequest> requests;
    const auto accepted = f.capture.uiEvent(7, "gain", ParamEventKind::Value, 0.875, f.ownerTime(), requests);
    check(!accepted && requests.empty() && f.probe->count == calls &&
              f.capture.appliedDeliveryStatus().state == AppliedDeliveryState::Invalidated,
          label + ": invalidated capture refuses input without touching the old instance");
}

void sourceBoundaries() {
    {
        Fixture left, right;
        const auto leftCommit = OpJournal(*left.store).commit(parameter(0.75));
        const auto rightCommit = OpJournal(*right.store).commit(parameter(0.25));
        check(leftCommit.ok && rightCommit.ok, "two independent sources commit the same numeric target ID");
        if (!leftCommit.ok || !rightCommit.ok) return;
        left.ownerDrain("left source only");
        left.sound(0.375, "left source reaches its own Session");
        right.sound(0.25, "right source stays unchanged before its own delivery");
        right.ownerDrain("right source only");
        right.sound(0.125, "right source reaches its own Session");
        left.sound(0.375, "right source never leaks across the shared numeric ID");
    }
    {
        Fixture f;
        const auto commit = OpJournal(*f.store).commit(parameter(0.75));
        check(commit.ok, "source-close fixture has owned pending delivery");
        if (!commit.ok) return;
        const auto calls = f.probe->count;
        f.store.reset();
        const auto invalidated = f.capture.retryApplied(f.ownerTime());
        check(invalidated.state == AppliedDeliveryState::Invalidated && invalidated.pendingBatchCount == 0 &&
                  !invalidated.error.empty() && f.probe->count == calls,
              "Store destruction invalidates queued work without retaining or dereferencing SQL");
        f.sound(0.25, "Session still renders safely after its Store has been destroyed");
    }
    {
        Fixture f;
        if (!f.commit(parameter(0.75), "dirty Save As live baseline")) return;
        auto* original = f.device;
        const auto pending = OpJournal(*f.store).commit(parameter(0.25));
        check(pending.ok, "dirty Save As has pending old-source runtime work");
        if (!pending.ok) return;
        const auto calls = f.probe->count;
        const auto saved = f.store->saveAs(f.directory.path() / "saved.adi");
        check(saved.ok, "dirty Save As replaces the Store connection");
        if (!saved.ok) return;
        const auto invalidated = f.capture.retryApplied(f.ownerTime());
        check(invalidated.state == AppliedDeliveryState::Invalidated && invalidated.pendingBatchCount == 0 &&
                  f.probe->count == calls && storedGain(*f.store) == 0.25,
              "discarded old-source receipt is reported while saved SQL already contains the newer gain");
        f.sound(0.375, "dirty Save As immediately retains the older audible value");
        require(f.session.refresh(*f.store), "refresh same Session on dirty saved connection");
        f.capture.attachSession(f.session);
        f.capture.attachSession(f.session);
        const auto stillInvalid = f.capture.retryApplied(f.ownerTime());
        check(stillInvalid.state == AppliedDeliveryState::Invalidated && !stillInvalid.error.empty() &&
                  f.session.instanceFor(7) == original && f.probe->count == calls && storedGain(*f.store) == 0.25,
              "same Session refresh and repeated attach cannot certify divergent old runtime as current");
        blockedCapture(f, "dirty Save As recovery requires a new Session");
        f.sound(0.375, "blocked dirty continuation never replays the discarded .25 receipt into the old instance");

        Session fresh;
        fresh.graph().setFadeFrames(0);
        fresh.setSourcesFor([&](std::int64_t id) { return id == 1 ? std::vector<Node*>{&f.source} : std::vector<Node*>{}; });
        require(fresh.load(*f.store, f.loader(), f.session.spec()), "load current saved SQL in a fresh Session");
        fresh.transport().play();
        f.capture.attachSession(fresh);
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Idle && f.loaderCalls == 2 &&
                  fresh.instanceFor(7) != original && original->getParam("gain").normalized == 0.75 &&
                  f.device->getParam("gain").normalized == 0.25,
              "fresh Session restores current SQL truth while the abandoned instance keeps its old value");
        render(fresh, f.precision, f.irregular, 0.125, f.clock, "fresh Session sounds the saved .25 gain before any new edit");
        if (!f.commit(parameter(0.625), "edit after fresh saved-connection recovery")) return;
        render(fresh, f.precision, f.irregular, 0.3125, f.clock, "new edit reaches the recovered Session");
        check(original->getParam("gain").normalized == 0.75,
              "new connection edits never reach the abandoned old instance");
    }
    {
        Fixture f;
        if (!f.commit(parameter(0.5), "clean Save As recorded baseline")) return;
        auto* original = f.device;
        std::vector<OpRequest> open;
        require(f.capture.uiEvent(7, "gain", ParamEventKind::Begin, 0.5, f.ownerTime(), open), "begin capture before Save As fence");
        require(f.capture.uiEvent(7, "gain", ParamEventKind::Value, 0.75, f.ownerTime(), open), "move capture before Save As fence");
        f.device->ownGesture(1, 0.125);
        const auto source = appliedSource(*f.store);
        const auto head = OpJournal(*f.store).headSeq();
        const auto calls = f.probe->count;
        check(open.empty(), "open UI gesture and queued editor gesture retain ownership before Save As");
        f.sound(0.5, "open UI and queued editor values remain audible");
        const auto refused = f.store->saveAs(f.directory.path() / "blocked.adi");
        check(!refused.ok && !f.store->close() && appliedSource(*f.store).sameConnection(source) &&
                  OpJournal(*f.store).headSeq() == head && storedGain(*f.store) == 0.5 && f.probe->count == calls,
              "open and queued capture refuses Save As and explicit Close without replacing source or runtime");
        std::vector<OpRequest> queued; f.capture.drain(f.ownerTime(), queued);
        check(queued.size() == 2 && queued.front().payload.at("param") == "bias" &&
                  queued.front().payload.at("norm") == 0 && queued.back().payload.at("norm") == 0.125,
              "refused Save As preserves queued editor gesture and first-touch origin");
        if (!f.commit(queued, "queued editor after refused Save As")) return;
        require(f.capture.uiEvent(7, "gain", ParamEventKind::End, 0.75, f.ownerTime(), open), "finish retained UI capture");
        check(open.size() == 1 && open.front().payload.at("norm") == 0.75,
              "refused Save As retains open UI gesture until its original End");
        if (!f.commit(open, "retained UI after refused Save As")) return;
        const auto saved = f.store->saveAs(f.directory.path() / "clean.adi");
        check(saved.ok, "quiescent completed capture permits clean Save As");
        if (!saved.ok) return;
        require(f.session.refresh(*f.store), "refresh clean Save As successor");
        f.capture.attachSession(f.session); f.capture.attachSession(f.session);
        check(f.capture.appliedDeliveryStatus().state == AppliedDeliveryState::Idle && f.session.instanceFor(7) == original &&
                  f.loaderCalls == 1 && f.probe->count == calls && storedGain(*f.store) == 0.75,
              "quiescent proven successor keeps live instance and fully committed capture state");
        f.sound(0.5, "clean continuation preserves committed parameter image");
        if (!f.undo("retained UI gesture Undo")) return;
        f.sound(0.375, "retained UI Undo restores pre-gesture gain");
        if (!f.redo("retained UI gesture Redo")) return;
        f.sound(0.5, "retained UI Redo restores accepted gain");
        if (!f.commit(parameter(0.625), "future edit after clean Save As")) return;
        f.sound(0.4375, "future delivery continues on same saved instance");
        f.sameInstance(original, "quiescent clean Save As continuation");
    }
    {
        Fixture f;
        if (!f.commit(parameter(0.75), "replacement-relation isolation baseline")) return;
        StoreError error{};
        auto other = Store::createEmpty(f.directory.path() / "zero-sequence-unrelated.adi", error);
        require(static_cast<bool>(other), "create zero-sequence unrelated source"); seedProject(*other);
        other->db().exec("INSERT INTO plugin_params(device_id,param_id,normalized_value,real_value,display) VALUES(7,'gain',0.75,NULL,'')");
        const auto calls = f.probe->count;
        const auto saved = f.store->saveAs(f.directory.path() / "relation-successor.adi");
        check(saved.ok, "clean Save As establishes eligibility before testing an unrelated zero-sequence source");
        if (!saved.ok) return;
        require(f.session.refresh(*other), "refresh unrelated matching source after clean Save As");
        check(f.session.appliedNotifications().sequence() == 0 && f.session.realisedAppliedSequence() == 0 &&
                  storedGain(*other) == 0.75 && f.device->getParam("gain").normalized == 0.75,
              "unrelated source has matching target values and no unseen publications to explain refusal");
        f.capture.attachSession(f.session);
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Invalidated && f.probe->count == calls,
              "exact Save As replacement relation alone rejects the unrelated zero-sequence connection");
        f.sound(0.375, "relation-only refusal preserves the already-matching runtime image");
        blockedCapture(f, "unrelated zero-sequence source after clean Save As");
    }
    {
        Fixture f;
        if (!f.commit(parameter(0.75), "unrelated Store baseline")) return;
        StoreError error{};
        auto other = Store::createEmpty(f.directory.path() / "unrelated.adi", error);
        require(static_cast<bool>(other), "create unrelated Store with identical numeric device IDs"); seedProject(*other);
        require(OpJournal(*other).commit(parameter(0.25)).ok, "seed unrelated current gain");
        const auto calls = f.probe->count;
        require(f.session.refresh(*other), "refresh existing Session from unrelated Store");
        f.capture.attachSession(f.session);
        f.capture.attachSession(f.session);
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Invalidated &&
                  f.probe->count == calls && storedGain(*other) == 0.25,
              "unrelated Store refresh cannot supply Save As successor authority");
        f.sound(0.375, "unrelated refreshed SQL does not silently replace the old live value");
        blockedCapture(f, "unrelated Store continuation");
    }
    {
        Fixture f;
        if (!f.commit(parameter(0.75), "unseen successor publication baseline")) return;
        const auto saved = f.store->saveAs(f.directory.path() / "unseen.adi");
        check(saved.ok, "clean Save As successor exists before an unseen new-domain commit");
        if (!saved.ok) return;
        const auto unseen = OpJournal(*f.store).commit(parameter(0.25));
        check(unseen.ok, "new-domain parameter commit occurs before delivery reattaches");
        if (!unseen.ok) return;
        const auto calls = f.probe->count;
        require(f.session.refresh(*f.store), "refresh after unseen successor publication");
        f.capture.attachSession(f.session);
        f.capture.attachSession(f.session);
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Invalidated &&
                  f.probe->count == calls && storedGain(*f.store) == 0.25,
              "direct successor provenance cannot excuse an unseen committed batch before attachment");
        f.sound(0.375, "unseen successor commit leaves visibly stale runtime until explicit recovery");
        blockedCapture(f, "unseen successor publication");
    }
    {
        Fixture f;
        if (!f.commit(parameter(0.75), "same-path reopen baseline")) return;
        const auto path = f.directory.path() / "song.adi";
        const auto calls = f.probe->count;
        f.store.reset();
        StoreError error{}; f.store = Store::open(path, error);
        require(static_cast<bool>(f.store), "reopen identical filename with a new Store connection");
        require(f.session.refresh(*f.store), "refresh same Session from reopened path");
        f.capture.attachSession(f.session);
        check(f.capture.retryApplied(f.ownerTime()).state == AppliedDeliveryState::Invalidated &&
                  f.probe->count == calls && storedGain(*f.store) == 0.75,
              "equal file path and matching rows do not prove connection continuation");
        f.sound(0.375, "same-path reopen preserves audible value while correctly refusing false continuity");
        blockedCapture(f, "same-path reopen");
    }
}

void sessionAndCaptureLifetimes() {
    test::TempDirectory directory("param_live_replay", "lifetimes");
    StoreError error{};
    auto store = Store::createEmpty(directory.path() / "song.adi", error);
    require(static_cast<bool>(store), "create Session lifetime fixture"); seedProject(*store);
    ConstantSource source;
    auto probe = std::make_shared<Probe>();
    RuntimeDevice* device = nullptr;
    const DeviceLoader loader = [&](const DeviceRequest&, std::string&) -> std::unique_ptr<device::DeviceInstance> {
        auto result = std::make_unique<RuntimeDevice>(probe); device = result.get(); return result;
    };
    auto session = std::make_unique<Session>();
    session->graph().setFadeFrames(0);
    session->setSourcesFor([&](std::int64_t id) { return id == 1 ? std::vector<Node*>{&source} : std::vector<Node*>{}; });
    SessionSpec spec; spec.channels = 2; spec.maxFrames = 64; spec.sampleRate = 48000;
    require(session->load(*store, loader, spec) && device, "load Session lifetime fixture");
    auto capture = std::make_unique<ParamOps>(); capture->attachSession(*session);
    const auto committed = OpJournal(*store).commit(parameter(0.75));
    check(committed.ok, "Session lifetime fixture has pending work");
    if (!committed.ok) return;
    const auto calls = probe->count;
    session.reset(); device = nullptr;
    const auto invalidated = capture->retryApplied(nowMs());
    check(invalidated.state == AppliedDeliveryState::Invalidated && invalidated.pendingBatchCount == 0 &&
              probe->destroyed == 1 && probe->count == calls,
          "Session-before-ParamOps destruction revokes work before any stale device dereference");
    std::vector<OpRequest> out; capture->drain(nowMs(), out);
    check(out.empty(), "drain after Session destruction neither touches instances nor manufactures edits");
    session = std::make_unique<Session>();
    require(session->load(*store, loader, spec) && device, "load replacement Session lifetime fixture");
    const auto replacementCalls = probe->count;
    capture->attachSession(*session);
    const auto rebound = capture->retryApplied(nowMs());
    check(rebound.state == AppliedDeliveryState::Idle && probe->count == replacementCalls &&
              device->getParam("gain").normalized == 0.75,
          "new Session loads current SQL once and never consumes the old instance's pending receipt");
    const auto newEdit = OpJournal(*store).commit(parameter(0.625));
    check(newEdit.ok && capture->retryApplied(nowMs()).state == AppliedDeliveryState::Idle &&
              device->getParam("gain").normalized == 0.625,
          "explicit attachment establishes a fresh delivery connection for the replacement Session");

    // A public frontend can go away while its cancellation is refused. The
    // Session keeps the runtime restoration task; its tick is the retry owner.
    device->ownGesture(0, 0.875);
    std::vector<OpRequest> abandoned; capture->drain(nowMs(), abandoned);
    check(!abandoned.empty() && !capture->pendingGestures().empty(), "frontend lifetime fixture owns an uncommitted live gesture");
    device->refuseParameters = true;
    capture.reset();
    session->tick(nowMs());
    check(device->getParam("gain").normalized == 0.875 && !History(*store).undo().ok,
          "destroyed frontend transfers refused restoration and source lease to its native Session");
    device->refuseParameters = false;
    session->tick(nowMs()); session->tick(nowMs());
    check(device->getParam("gain").normalized == 0.625 && storedGain(*store) == 0.625,
          "Session owner tick finishes abandoned restoration without a live ParamOps frontend");
    const auto before = probe->count;
    const auto later = OpJournal(*store).commit(parameter(0.25));
    check(later.ok && probe->count == before && device->getParam("gain").normalized == 0.625, "destroyed ParamOps leaves no live observer calling its former instance");
    device->ownGesture(0, 0.875);
    check(device->getParam("gain").normalized == 0.875, "former producer can broadcast safely after ParamOps destruction");
    session.reset(); device = nullptr;
    check(probe->destroyed == 2, "each Session-owned device is destroyed exactly once");

    session = std::make_unique<Session>();
    require(session->load(*store, loader, spec) && device, "load direct destruction-order fixture");
    capture = std::make_unique<ParamOps>(); capture->attachSession(*session);
    const auto pending = OpJournal(*store).commit(parameter(0.75));
    check(pending.ok, "direct destruction order includes an undrained receipt");
    const auto finalCalls = probe->count;
    session.reset(); device = nullptr;
    capture.reset();
    check(probe->destroyed == 3 && probe->count == finalCalls,
          "Session then ParamOps destruction is safe without an intervening drain or explicit detach");
}

void documentPath() {
    test::TempDirectory directory("param_live_replay", "document");
    StoreError status{}; auto store = Store::createEmpty(directory.path() / "document.adi", status);
    require(static_cast<bool>(store), "create document runtime fixture"); seedProject(*store); store.reset();
    ConstantSource source;
    auto probe = std::make_shared<Probe>();
    RuntimeDevice* device = nullptr;
    unsigned loaded = 0;
    std::string error;
    auto document = ui::ProjectDocument::open(directory.path() / "document.adi", false,
        [&](const DeviceRequest&, std::string&) -> std::unique_ptr<device::DeviceInstance> {
            ++loaded; auto instance = std::make_unique<RuntimeDevice>(probe); device = instance.get(); return instance;
        }, error);
    check(document && device, "unchanged ProjectDocument opens and attaches its ParamOps: " + error);
    if (!document || !device) return;
    auto* original = device;
    document->session().graph().setFadeFrames(0);
    document->session().setSourcesFor([&](std::int64_t id) { return id == 1 ? std::vector<Node*>{&source} : std::vector<Node*>{}; });
    document->prepare(48000, 64); document->session().transport().play();
    std::int64_t clock = 0;
    render(*document, Precision::Float32, true, 0.25, clock, "document initial PCM");
    check(document->parameterGesture(7, "gain", ParamEventKind::Begin, 0.5) &&
              document->parameterGesture(7, "gain", ParamEventKind::Value, 0.75) &&
              document->parameterGesture(7, "gain", ParamEventKind::End, 0.75),
          "existing document gesture path commits opener and final value");
    document->tick(nowMs()); render(*document, Precision::Float32, true, 0.375, clock, "document committed gesture");
    check(document->ops().undo().ok && document->synchronise(), "existing document submitter Undo and synchronise");
    document->tick(nowMs()); render(*document, Precision::Float32, true, 0.25, clock, "document Undo reaches live plug-in");
    check(parameterRows(document->store()) == 0, "document Undo clears first parameter row");
    check(document->ops().redo().ok && document->synchronise(), "existing document submitter Redo and synchronise");
    document->tick(nowMs()); render(*document, Precision::Float32, true, 0.375, clock, "document Redo reaches live plug-in");
    check(document->ops().undo().ok && document->synchronise(), "document second Undo"); document->tick(nowMs());
    const auto count = OpJournal(document->store()).count();
    check(document->parameterGesture(7, "gain", ParamEventKind::Begin, 0.5) &&
              document->parameterGesture(7, "gain", ParamEventKind::Value, 0.625) &&
              document->parameterGesture(7, "gain", ParamEventKind::End, 0.625),
          "document next gesture after cleared opener succeeds");
    document->tick(nowMs()); render(*document, Precision::Float32, true, 0.3125, clock, "document next gesture PCM");
    check(OpJournal(document->store()).count() == count + 2 && loaded == 1 && device == original &&
              !probe->ownerCallOnAudio && !probe->overflow,
          "unchanged document wiring emits a fresh opener and retains the actual instance");
    const auto fifoCount = OpJournal(document->store()).count();
    device->ownGesture(0, 0.75); device->ownGesture(0, 0.875);
    const auto calls = probe->count;
    document->tick(nowMs());
    check(OpJournal(document->store()).count() == fifoCount + 1 && storedGain(document->store()) == 0.75 &&
              probe->count == calls, "unchanged Document tick commits exactly the first completed FIFO gesture");
    render(*document, Precision::Float32, true, 0.4375, clock, "Document older receipt preserves newer accepted input");
    check(!document->ops().undo().ok, "Document History remains fenced while later completed input is owned");
    document->tick(nowMs());
    check(OpJournal(document->store()).count() == fifoCount + 2 && storedGain(document->store()) == 0.875 &&
              probe->count == calls, "next unchanged Document tick commits only the second FIFO gesture");
    check(document->ops().undo().ok && document->synchronise(), "Document later FIFO Undo"); document->tick(nowMs());
    render(*document, Precision::Float32, true, 0.375, clock, "Document later FIFO Undo preserves first boundary");
    check(document->ops().undo().ok && document->synchronise(), "Document earlier FIFO Undo"); document->tick(nowMs());
    render(*document, Precision::Float32, true, 0.3125, clock, "Document earlier FIFO Undo restores committed origin");
    device->preset({0.5, 0.125, 6});
    document->tick(nowMs());
    check(stateRows(document->store()) == 1 && document->store().db().execAndGet("SELECT count(*) FROM state_blobs").getInt64() >= 2,
              "unchanged writeBlobs then ordinary journal bridge atomically commits preset resources");
    render(*document, Precision::Float32, true, 0.375, clock, "Document captured preset sound");
    check(document->ops().undo().ok && document->synchronise(), "Document captured preset Undo"); document->tick(nowMs());
    render(*document, Precision::Float32, true, 0.3125, clock, "Document preset Undo restores exact pre-state value");
    const auto failedCount = OpJournal(document->store()).count();
    const auto failedHead = OpJournal(document->store()).headSeq();
    const auto failedBlobs = document->store().db().execAndGet("SELECT count(*) FROM state_blobs").getInt64();
    check(stateRows(document->store()) == 0 && storedGain(document->store()) == 0.625,
          "Document preset Undo leaves absent state and the exact earlier committed parameter before refusal");
    // Scratch applies real state handlers but never appends journal rows. Fail
    // the actual commit after resources and material were written so this is
    // transaction rollback, not an earlier retained preparation refusal.
    document->store().db().exec("CREATE TRIGGER reject_document_preset BEFORE INSERT ON ops WHEN NEW.op_type='device.loadState' BEGIN SELECT RAISE(ABORT,'document preset refusal'); END");
    device->preset({0.75, 0.0625, 11});
    document->tick(nowMs());
    check(document->error().find("document preset refusal") != std::string::npos && OpJournal(document->store()).count() == failedCount &&
              OpJournal(document->store()).headSeq() == failedHead && stateRows(document->store()) == 0 &&
              document->store().db().execAndGet("SELECT count(*) FROM state_blobs").getInt64() == failedBlobs,
              "unchanged Document bridge rolls staged blobs back with a refused state transaction: " + document->error());
    document->store().db().exec("DROP TRIGGER reject_document_preset");
    document->tick(nowMs());
    check(OpJournal(document->store()).count() == failedCount && !document->ops().undo().ok,
              "Document tick retains refusal and lease instead of silently retrying or dropping the captured preset");
    render(*document, Precision::Float32, true, 0.4375, clock, "Document refused preset remains audible and retained");
}
} // namespace

int main() {
    try {
        openerAndPresence(); agentAndPluginEditor(); stateAndParameterProjection(); deferredAndEchoes();
        rollbackDoesNotDeliver(); branchesAndSnapshot(); ownReceiptAcknowledgement(); pendingCaptureBoundary(); completeCaptureAcknowledgement(); capturedOwnershipAndResources(); concurrentCaptureBindings(); refusedCapturedFifo(); refusedParameterAndFifo(); refusedOpeners();
        refusedFinalProjection(); clearOnlyInverseBoundary(); attachmentAndRetirement();
        pinnedStateBytes(); sourceBoundaries(); sessionAndCaptureLifetimes(); documentPath();
    } catch (const std::exception& exception) {
        check(false, std::string("unexpected fixture exception: ") + exception.what());
    }
    std::printf("parameter live replay: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
