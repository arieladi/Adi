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
            // inside the witness's epoch read, before it samples pushed count.
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
        setPatch(value); pendingValid_.fill(false); ++epoch_;
        if (broadcasts) {
            broadcastParam(0, ParamEventKind::Value, value.gain);
            broadcastParam(1, ParamEventKind::Value, value.bias);
        }
    }
    void stateEcho() noexcept { ++epoch_; }
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
        auto requests = f.gesture(0.75);
        require(requests.size() == 2, "tampered receipt fixture has complete opener group");
        requests.back().payload["norm"] = 0.625;
        const auto before = f.probe->sets;
        if (!f.commit(requests, "tampered copied receipt")) return;
        check(f.probe->sets >= before + 2, "copied proof cannot acknowledge changed payloads; literal opener and edit execute");
        f.sound(0.3125, "tampered group's actual committed payload controls sound");
    }
    {
        Fixture f;
        const auto requests = f.gesture(0.75);
        require(requests.size() == 2, "partial receipt fixture has opener group");
        const auto before = f.probe->sets;
        if (!f.commit(requests.front(), "only opener from copied group")) return;
        check(f.probe->sets > before, "missing group member prevents self-acknowledgement");
        f.sound(0.25, "incomplete group executes the sole committed opener literally");
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
        const auto stale = f.gesture(0.75);
        if (!f.undo("intervening history before own commit")) return;
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
        const auto stale = f.gesture(0.75);
        require(stale.size() == 2, "sequence-only stale proof retains its complete first-touch group");
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
              "publication sequence alone rejects stale self-acknowledgement and executes the literal opener plus final value");
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
        // No state acknowledgement is pending. The first epoch read chooses
        // ordinary draining; the second is the complete interval's witness.
        // The hook publishes after the consumer boundary but before the
        // witness samples pushed count, so a later counter comparison alone
        // cannot distinguish the newer still-queued input.
        f.device->injectGestureOnEpochRead(2, 0.875);
        std::vector<OpRequest> older; f.capture.drain(f.ownerTime(), older);
        const auto* captured = f.capture.captureStats(7);
        check(f.device->epochGestureInjections() == 1 && captured && captured->pushed == 6 &&
                  older.size() == 2 && older.front().payload.at("norm") == 0.5 &&
                  older.back().payload.at("norm") == 0.75,
              "witness-boundary hook queues newer input while only the older gesture is emitted");
        const auto committed = OpJournal(*f.store).commit(older);
        check(committed.ok, "older witness-boundary interval commits normally");
        if (!committed.ok) return;
        const auto calls = f.probe->count;
        const auto delivered = f.capture.retryApplied(f.ownerTime());
        const auto* after = f.capture.captureStats(7);
        check(delivered.state == AppliedDeliveryState::Idle && after && after->pushed == 6 && f.probe->count == calls,
              "queued newer input survives own acknowledgement even when its push is already in the witness counter");
        f.sound(0.4375, "queued newer gesture stays audible across older self acknowledgement");
        std::vector<OpRequest> newer; f.capture.drain(f.ownerTime(), newer);
        check(newer.size() == 1 && newer.front().payload.at("norm") == 0.875,
              "later drain emits the still-pending newer gesture exactly once");
        if (!f.commit(newer, "later witness-boundary gesture")) return;
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
        check(newer.empty(), "newer explicit UI gesture remains open before its End");
        const auto before = f.probe->count;
        if (!f.commit(older, "older UI receipt with a newer open UI gesture")) return;
        check(f.probe->count == before,
              "older own acknowledgement observes newer accepted UI input without replaying the old opener");
        f.sound(0.4375, "newer open UI value stays audible through older commit acknowledgement");
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
        std::vector<OpRequest> requests; f.capture.drain(f.ownerTime(), requests);
        std::vector<double> values;
        for (const auto& request : requests) {
            if (request.opType == "device.setParam") values.push_back(request.payload.at("norm").get<double>());
        }
        check(values == std::vector<double>{0.5, 0.75, 0.625},
              "one drain preserves two completed gestures and their single first-touch opener");
        const auto before = f.probe->count;
        if (!f.commit(requests, "two editor gestures in one capture interval")) return;
        check(f.probe->count == before,
              "complete capture acknowledgement does not replay earlier intermediate gesture values");
        f.sound(0.3125, "one captured interval retains its final actual editor value");
        if (!f.undo("combined gestures Undo")) return;
        check(f.probe->count > before && parameterRows(*f.store) == 0,
              "combined gesture Undo executes literal inverses and clears the opener");
        f.sound(0.25, "combined gesture Undo restores the actual interval origin");
        const auto undoCalls = f.probe->count;
        if (!f.redo("combined gestures Redo")) return;
        check(f.probe->count > undoCalls, "combined gesture Redo executes the stored values again");
        f.sound(0.3125, "combined gesture Redo restores the final editor value");
    }
    {
        Fixture f(Precision::Float64, true);
        f.device->ownGesture(0, 0.75);
        f.device->preset({0.625, 0.125, 4});
        std::vector<OpRequest> requests; f.capture.drain(f.ownerTime(), requests);
        std::vector<double> values;
        std::size_t states = 0;
        for (const auto& request : requests) {
            if (request.opType == "device.setParam") values.push_back(request.payload.at("norm").get<double>());
            if (request.opType == "device.loadState") ++states;
        }
        check(values == std::vector<double>{0.5, 0.75, 0.625} && states == 2,
              "one drain preserves explicit gesture, state opener, preset and its parameter row-follow");
        std::string error;
        require(f.capture.writeBlobs(*f.store, error), "write combined gesture and preset resources");
        const auto before = f.probe->count;
        if (!f.commit(requests, "explicit editor gesture then preset in one interval")) return;
        check(f.probe->count == before,
              "complete mixed capture acknowledgement adds no parameter setter or state reload");
        f.sound(0.4375, "mixed interval's final preset remains audible");
        if (!f.undo("combined gesture and preset Undo")) return;
        check(f.probe->count > before && parameterRows(*f.store) == 0 && stateRows(*f.store) == 0,
              "mixed interval Undo executes literal inverses and clears both first-touch rows");
        f.sound(0.25, "mixed interval Undo restores the attach-time sound");
        const auto undoSets = f.probe->sets, undoLoads = f.probe->loads;
        if (!f.redo("combined gesture and preset Redo")) return;
        check(f.probe->sets > undoSets && f.probe->loads > undoLoads,
              "mixed interval Redo performs actual parameter and state delivery");
        f.sound(0.4375, "mixed interval Redo restores the final preset");
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
              "known retired target acknowledges old work without retrying its obsolete instance");
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
        require(f.capture.uiEvent(7, "gain", ParamEventKind::Begin, 0.5, f.ownerTime(), open), "begin UI capture across clean Save As");
        require(f.capture.uiEvent(7, "gain", ParamEventKind::Value, 0.75, f.ownerTime(), open), "move UI capture across clean Save As");
        f.device->ownGesture(1, 0.125);
        check(open.empty(), "clean Save As begins with an open UI gesture and a separate queued editor gesture");
        const auto calls = f.probe->count;
        f.sound(0.5, "accepted open UI and queued editor values are audible before clean Save As");
        const auto saved = f.store->saveAs(f.directory.path() / "clean.adi");
        check(saved.ok, "clean Save As succeeds without pending committed delivery");
        if (!saved.ok) return;
        require(f.session.refresh(*f.store), "refresh clean direct Save As successor");
        f.capture.attachSession(f.session);
        f.capture.attachSession(f.session);
        check(f.capture.appliedDeliveryStatus().state == AppliedDeliveryState::Idle && f.session.instanceFor(7) == original &&
                  f.loaderCalls == 1 && f.probe->count == calls && storedGain(*f.store) == 0.5,
              "proven clean Save As keeps the same instance and live capture without reload or SDK calls");
        f.sound(0.5, "clean continuation preserves live producer values before their later commits");
        std::vector<OpRequest> queued; f.capture.drain(f.ownerTime(), queued);
        check(queued.size() == 2 && queued.front().payload.at("param") == "bias" &&
                  queued.front().payload.at("norm") == 0 && queued.back().payload.at("norm") == 0.125,
              "clean source rotation retains the queued editor ring and its exact first-touch origin");
        if (!f.commit(queued, "queued editor gesture after clean Save As")) return;
        require(f.capture.uiEvent(7, "gain", ParamEventKind::End, 0.75, f.ownerTime(), open), "finish retained UI capture after Save As");
        check(open.size() == 1 && open.front().payload.at("norm") == 0.75,
              "clean source rotation retains the open UI gesture until its original End");
        if (!f.commit(open, "retained UI gesture after clean Save As")) return;
        check(f.probe->count == calls, "own captured values acknowledge across clean Save As without redundant setters");
        if (!f.undo("retained UI gesture Undo")) return;
        f.sound(0.375, "retained UI gesture Undo restores its pre-Save As origin");
        if (!f.redo("retained UI gesture Redo")) return;
        f.sound(0.5, "retained UI gesture Redo restores its accepted final value");
        if (!f.commit(parameter(0.625), "future parameter edit after clean Save As")) return;
        f.sound(0.4375, "ordinary future delivery continues on the same saved instance");
        f.sameInstance(original, "clean Save As capture continuation");
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

    // The ordinary ownership order is independent: detach the subscription and
    // callback sink while the Session-owned instance is still alive.
    const auto before = probe->count;
    capture.reset();
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
}
} // namespace

int main() {
    try {
        openerAndPresence(); agentAndPluginEditor(); stateAndParameterProjection(); deferredAndEchoes();
        rollbackDoesNotDeliver(); branchesAndSnapshot(); ownReceiptAcknowledgement(); pendingCaptureBoundary(); completeCaptureAcknowledgement(); refusedParameterAndFifo(); refusedOpeners();
        refusedFinalProjection(); clearOnlyInverseBoundary(); attachmentAndRetirement();
        pinnedStateBytes(); sourceBoundaries(); sessionAndCaptureLifetimes(); documentPath();
    } catch (const std::exception& exception) {
        check(false, std::string("unexpected fixture exception: ") + exception.what());
    }
    std::printf("parameter live replay: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
