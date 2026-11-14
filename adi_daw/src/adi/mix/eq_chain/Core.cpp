// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#include "../AlignedKey.hpp"
#include "../eq/Core.hpp"
#include "../carve/Core.hpp"
#include "../focus/Core.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace adi::mix::eq_chain {
namespace {
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<void *>::is_always_lock_free);
std::atomic<std::uint64_t> nextEpoch{1};

// One serialized writer, bounded readers. A reader leases a complete immutable
// slot, then checks its serial to reject a slot reused under an old identity.
template <class Value> class Mailbox {
    struct Slot {
        std::atomic<int> readers{0};
        Value value{};
        std::uint64_t version = 0;
    };
    mutable std::array<Slot, 3> slots_;
    std::atomic<std::uint64_t> published_{0};
    std::uint64_t serial_ = 0;

  public:
    std::uint64_t publish(const Value &value) noexcept {
        const auto current = published_.load(std::memory_order_acquire) & 3U;
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (index == current)
                continue;
            auto &slot = slots_[index];
            int zero = 0;
            if (!slot.readers.compare_exchange_strong(zero, -1, std::memory_order_acq_rel))
                continue;
            slot.value = value;
            slot.version = ++serial_;
            slot.readers.store(0, std::memory_order_release);
            published_.store((serial_ << 2U) | index, std::memory_order_release);
            return serial_;
        }
        return 0;
    }
    bool read(Value &value, std::uint64_t &version) const noexcept {
        const auto identity = published_.load(std::memory_order_acquire);
        if (identity == 0)
            return false;
        auto &slot = slots_[static_cast<std::size_t>(identity & 3U)];
        int readers = slot.readers.load(std::memory_order_relaxed);
        for (int attempt = 0; attempt < 3 && readers >= 0; ++attempt) {
            if (slot.readers.compare_exchange_strong(readers, readers + 1, std::memory_order_acquire)) {
                const bool current = slot.version == (identity >> 2U);
                if (current) {
                    value = slot.value;
                    version = slot.version;
                }
                slot.readers.fetch_sub(1, std::memory_order_release);
                return current;
            }
        }
        return false;
    }
};
bool downstream(const Settings &s) noexcept { return s.carveEnabled || s.focusEnabled; }
struct KeyStorage {
    AlignedKey<float> single;
    AlignedKey<double> dual;
    void prepare(int maximumDelay, int frames) {
        single.prepare(maximumDelay, frames);
        dual.prepare(maximumDelay, frames);
    }
    void clear() noexcept {
        single.clear();
        dual.clear();
    }
};
struct Candidate {
    Settings settings;
    std::uint64_t eqToken = 0, focusRequested = 0;
    int eqLatency = 0;
};
struct Ready {
    PlanOffer plan;
    std::uint64_t keyEpoch = 0, focusOfferToken = 0;
    bool primeEq = false;
};
// The owner writes only before readyVersion's release publication, then leaves
// every field untouched until the sole audio publisher acknowledges that version.
// Audio cannot acknowledge before its complete read. Owner reads are serialized
// with service; an advertised offer therefore has no fallible read lease.
// Atomic fields also avoid data races during an unadvertised publication.
struct ReadyFields {
    std::atomic<std::uint64_t> token{0}, eqToken{0}, focusReadyToken{0}, focusEpoch{0}, keyEpoch{0},
        focusOfferToken{0};
    std::atomic<int> totalLatency{0}, eqLatency{0}, focusLatency{0}, flags{0};
    void publish(const Ready &r) noexcept {
        token.store(r.plan.token, std::memory_order_relaxed);
        eqToken.store(r.plan.eqToken, std::memory_order_relaxed);
        focusReadyToken.store(r.plan.focusReadyToken, std::memory_order_relaxed);
        focusEpoch.store(r.plan.focusEpoch, std::memory_order_relaxed);
        keyEpoch.store(r.keyEpoch, std::memory_order_relaxed);
        focusOfferToken.store(r.focusOfferToken, std::memory_order_relaxed);
        totalLatency.store(r.plan.totalLatency, std::memory_order_relaxed);
        eqLatency.store(r.plan.eqLatency, std::memory_order_relaxed);
        focusLatency.store(r.plan.focusLatency, std::memory_order_relaxed);
        flags.store((r.plan.eqEnabled ? 1 : 0) | (r.plan.carveEnabled ? 2 : 0) |
                        (r.plan.focusEnabled ? 4 : 0) | (r.primeEq ? 8 : 0),
                    std::memory_order_relaxed);
    }
    Ready read() const noexcept {
        Ready r;
        r.plan.token = token.load(std::memory_order_relaxed);
        r.plan.eqToken = eqToken.load(std::memory_order_relaxed);
        r.plan.focusReadyToken = focusReadyToken.load(std::memory_order_relaxed);
        r.plan.focusEpoch = focusEpoch.load(std::memory_order_relaxed);
        r.keyEpoch = keyEpoch.load(std::memory_order_relaxed);
        r.focusOfferToken = focusOfferToken.load(std::memory_order_relaxed);
        r.plan.totalLatency = totalLatency.load(std::memory_order_relaxed);
        r.plan.eqLatency = eqLatency.load(std::memory_order_relaxed);
        r.plan.focusLatency = focusLatency.load(std::memory_order_relaxed);
        const auto bits = flags.load(std::memory_order_relaxed);
        r.plan.eqEnabled = (bits & 1) != 0;
        r.plan.carveEnabled = (bits & 2) != 0;
        r.plan.focusEnabled = (bits & 4) != 0;
        r.primeEq = (bits & 8) != 0;
        return r;
    }
};
} // namespace

bool valid(const Settings &s) noexcept {
    return eq::valid(s.eq) && carve::valid(s.carve) && focus::valid(s.focus);
}

struct Core::Impl {
    PrepareSpec spec{};
    eq::Core equalizer;
    // These pointers are created and destroyed only by the owner. Once created,
    // storage is retained until stopped destruction; no callback releases it.
    std::unique_ptr<carve::Core> carver;
    std::unique_ptr<focus::Core> focuser;
    std::unique_ptr<KeyStorage> keys;
    std::atomic<carve::Core *> publishedCarve{nullptr};
    std::atomic<focus::Core *> publishedFocus{nullptr};
    std::atomic<KeyStorage *> publishedKeys{nullptr};
    std::array<std::vector<float>, 2> zeroFloat;
    std::array<std::vector<double>, 2> zeroDouble;
    std::array<std::vector<float>, 2> scratchFloat;
    std::array<std::vector<double>, 2> scratchDouble;
    Mailbox<Candidate> requests;
    ReadyFields offers;
    std::atomic<std::uint64_t> readyVersion{0}, acknowledged{0};
    std::atomic<std::uint64_t> keySeenEpoch{0}, warmFrames{0};
    std::atomic<std::uint64_t> eqPrimedVersion{0}, eqWarmFrames{0};
    std::atomic<std::uint64_t> focusPreparations{0}, eqResets{0};
    std::atomic<int> committedLatency{0};
    int maximumEqLatency = 0;
    // Audio ownership.
    Settings active{}, desired{};
    Candidate candidate{};
    std::uint64_t candidateVersion = 0, epoch = 0, generation = 1, serial = 0, physical = 0;
    std::uint64_t primeVersion = 0;
    PlanOffer activePlan{};
    Ready audioReady{};
    eq::BlockContext eqContext{};
    eq::BlockContext eqPrimeContext{};
    focus::BlockContext focusContext{};
    carve::Core *audioCarve = nullptr;
    focus::Core *audioFocus = nullptr;
    KeyStorage *audioKeys = nullptr;
    Statistics stats{};
    int keyLayout = 0, precision = 0;
    bool prepared = false, blockValid = false;
    // Owner ownership. A nonzero readyVersion remains immutable until acquire
    // observes acknowledgment. No disabled child is touched by audio afterward.
    Settings ownerActive{};
    Candidate ownerCandidate{};
    Ready ownerReady{};
    std::uint64_t buildingVersion = 0, lastBuiltVersion = 0, ownerAcknowledged = 0;
    std::uint64_t ownerKeyEpoch = 0, ownerFocusEpoch = 0;

    bool supported(const Settings &s) const noexcept {
        return valid(s) && (!s.carveEnabled || (spec.sampleRate >= 44100. && spec.sampleRate <= 384000.)) &&
               (!s.focusEnabled ||
                (spec.channels == 2 && spec.sampleRate >= 44100. && spec.sampleRate <= 192000.));
    }
    void prepare(PrepareSpec next, const Settings &s) {
        // Called only on a newly constructed Impl. Public prepare/reset build
        // a replacement first, then swap on the stopped owner; no mailbox,
        // latent plan, logical key layout or old audio pointer survives reuse.
        spec = next;
        if (!std::isfinite(spec.sampleRate) || spec.sampleRate < 8000. || spec.sampleRate > 768000. ||
            spec.maximumFrames < 1 || spec.maximumFrames > 65536 ||
            (spec.channels != 1 && spec.channels != 2) || !supported(s))
            throw std::invalid_argument("Invalid EQ chain specification or unsupported enabled stage");
        equalizer.prepare({spec.sampleRate, spec.maximumFrames, spec.channels}, s.eq);
        maximumEqLatency = equalizer.maximumLatencySamples();
        for (auto &v : zeroFloat)
            v.assign(static_cast<std::size_t>(spec.maximumFrames), 0.f);
        for (auto &v : zeroDouble)
            v.assign(static_cast<std::size_t>(spec.maximumFrames), 0.);
        for (auto &v : scratchFloat)
            v.resize(static_cast<std::size_t>(spec.maximumFrames));
        for (auto &v : scratchDouble)
            v.resize(static_cast<std::size_t>(spec.maximumFrames));
        active = desired = ownerActive = s;
        activePlan.eqEnabled = s.eqEnabled;
        activePlan.carveEnabled = s.carveEnabled;
        activePlan.focusEnabled = s.focusEnabled;
        activePlan.eqLatency = s.eqEnabled ? equalizer.latencySamples() : 0;
        if (s.eqEnabled)
            eqContext = equalizer.beginBlock(0, activePlan.eqLatency, s.eq, false);
        if (s.carveEnabled) {
            carver = std::make_unique<carve::Core>();
            carver->prepare({spec.sampleRate, spec.maximumFrames, spec.channels}, s.carve);
            audioCarve = carver.get();
            publishedCarve.store(audioCarve, std::memory_order_release);
        }
        if (s.focusEnabled) {
            focuser = std::make_unique<focus::Core>();
            focuser->prepare({spec.sampleRate, spec.maximumFrames, spec.channels}, s.focus);
            audioFocus = focuser.get();
            publishedFocus.store(audioFocus, std::memory_order_release);
            focusPreparations.store(1, std::memory_order_relaxed);
            ownerFocusEpoch = 1;
            activePlan.focusReadyToken = focuser->configurationStatus().ready;
            activePlan.focusEpoch = ownerFocusEpoch;
            activePlan.focusLatency = focuser->latencySamples();
            focusContext = focuser->beginBlock(0, activePlan.focusLatency, s.focus, false);
        }
        if (downstream(s)) {
            keys = std::make_unique<KeyStorage>();
            keys->prepare(maximumEqLatency, spec.maximumFrames);
            audioKeys = keys.get();
            publishedKeys.store(audioKeys, std::memory_order_release);
            ownerKeyEpoch = 1;
            keySeenEpoch.store(1, std::memory_order_relaxed);
            // At stream creation the prehistory is known silence, rather than
            // missing elapsed media from a disabled capture interval.
            warmFrames.store(static_cast<std::uint64_t>(maximumEqLatency), std::memory_order_relaxed);
        }
        activePlan.totalLatency = activePlan.eqLatency + activePlan.focusLatency;
        committedLatency.store(activePlan.totalLatency, std::memory_order_release);
        epoch = nextEpoch.fetch_add(1, std::memory_order_relaxed);
        prepared = true;
    }
    bool request(const Settings &s) noexcept {
        if (!prepared || !supported(s))
            return false;
        desired = s;
        if (candidateVersion || s == active)
            return true;
        Candidate next;
        next.settings = s;
        if (s.eqEnabled && active.eqEnabled) {
            if (!equalizer.requestConfiguration(s.eq))
                return false;
            const auto pending = equalizer.pendingLatencyChange();
            next.eqToken = pending.token;
            next.eqLatency = pending.token ? pending.samples : equalizer.latencySamples();
        }
        if (s.focusEnabled && active.focusEnabled) {
            if (!audioFocus->requestConfiguration(s.focus))
                return false;
            next.focusRequested = audioFocus->configurationStatus().requested;
        }
        const auto version = requests.publish(next);
        if (!version)
            return false;
        candidate = next;
        candidateVersion = version;
        return true;
    }
    bool service() {
        if (!prepared)
            return false;
        const auto ready = readyVersion.load(std::memory_order_acquire);
        const auto admitted = acknowledged.load(std::memory_order_acquire);
        if (ready && admitted != ready)
            return false;
        if (admitted != ownerAcknowledged) {
            ownerActive = ownerCandidate.settings;
            ownerAcknowledged = admitted;
        }
        Candidate next;
        std::uint64_t version = 0;
        if (!requests.read(next, version) || version == lastBuiltVersion)
            return false;
        if (buildingVersion != version) {
            ownerCandidate = next;
            ownerReady = {};
            auto &p = ownerReady.plan;
            const auto &s = next.settings;
            p.token = version;
            p.eqEnabled = s.eqEnabled;
            p.carveEnabled = s.carveEnabled;
            p.focusEnabled = s.focusEnabled;
            p.eqToken = next.eqToken;
            p.eqLatency = next.eqLatency;
            if (s.eqEnabled && !ownerActive.eqEnabled) {
                equalizer.reset(s.eq);
                eqResets.fetch_add(1, std::memory_order_relaxed);
                p.eqLatency = equalizer.latencySamples();
                p.eqToken = 0;
                ownerReady.primeEq = downstream(s);
            }
            if (s.carveEnabled && !carver) {
                auto incoming = std::make_unique<carve::Core>();
                incoming->prepare({spec.sampleRate, spec.maximumFrames, spec.channels}, s.carve);
                carver = std::move(incoming);
                publishedCarve.store(carver.get(), std::memory_order_release);
            }
            if (downstream(s)) {
                if (!keys) {
                    auto incoming = std::make_unique<KeyStorage>();
                    incoming->prepare(maximumEqLatency, spec.maximumFrames);
                    keys = std::move(incoming);
                    ++ownerKeyEpoch;
                    publishedKeys.store(keys.get(), std::memory_order_release);
                } else if (!downstream(ownerActive)) {
                    keys->clear();
                    ++ownerKeyEpoch;
                }
                ownerReady.keyEpoch = ownerKeyEpoch;
            }
            if (s.focusEnabled && !ownerActive.focusEnabled) {
                if (!focuser) {
                    auto incoming = std::make_unique<focus::Core>();
                    incoming->prepare({spec.sampleRate, spec.maximumFrames, spec.channels}, s.focus);
                    focuser = std::move(incoming);
                } else
                    focuser->reset(s.focus);
                ++ownerFocusEpoch;
                focusPreparations.fetch_add(1, std::memory_order_relaxed);
                publishedFocus.store(focuser.get(), std::memory_order_release);
            }
            buildingVersion = version;
        }
        auto &p = ownerReady.plan;
        if (next.settings.focusEnabled) {
            if (ownerActive.focusEnabled) {
                focuser->serviceConfiguration();
                // Audio may have coalesced newer scalar intent under the same
                // frozen structural plan. Freeze the generation actually
                // ready now, never an older generation than the candidate's
                // request. After this parent offer, service is held until ack.
                if (focuser->configurationStatus().ready < next.focusRequested)
                    return false;
            }
            const auto change = focuser->pendingLatencyChange();
            p.focusReadyToken = focuser->configurationStatus().ready;
            p.focusEpoch = ownerFocusEpoch;
            ownerReady.focusOfferToken = change.token;
            p.focusLatency = change.token ? change.samples : focuser->latencySamples();
        }
        p.totalLatency = p.eqLatency + p.focusLatency;
        offers.publish(ownerReady);
        lastBuiltVersion = version;
        readyVersion.store(version, std::memory_order_release);
        return true;
    }
    bool ready(Ready &r) const noexcept {
        const auto version = readyVersion.load(std::memory_order_acquire);
        if (!version || acknowledged.load(std::memory_order_acquire) == version)
            return false;
        r = offers.read();
        return r.plan.token == version && readyVersion.load(std::memory_order_acquire) == version &&
               acknowledged.load(std::memory_order_acquire) != version;
    }
    bool warm(const Ready &r) const noexcept {
        const auto needed = static_cast<std::uint64_t>(r.plan.eqLatency);
        const bool keyWarm = (!r.plan.carveEnabled && !r.plan.focusEnabled) || needed == 0 ||
                             (keySeenEpoch.load(std::memory_order_acquire) == r.keyEpoch &&
                              warmFrames.load(std::memory_order_acquire) >= needed);
        const bool eqWarm = !r.primeEq || needed == 0 ||
                            (eqPrimedVersion.load(std::memory_order_acquire) == r.plan.token &&
                             eqWarmFrames.load(std::memory_order_acquire) >= needed);
        return keyWarm && eqWarm;
    }
    PlanOffer pending() const noexcept {
        Ready r;
        return ready(r) && warm(r) ? r.plan : PlanOffer{};
    }
    Settings controls() const noexcept {
        auto result = desired;
        if (candidateVersion) {
            // Preserve the complete requested plan across all children. New
            // scalar controls remain live, while a subsequent structural edit
            // cannot get ahead of another child's immutable ready generation.
            result.eq = eq::pinPlanSettings(result.eq, candidate.settings.eq);
            result.focus = focus::pinConfiguration(result.focus, candidate.settings.focus);
        }
        return result;
    }
    bool validateReady(const Ready &r) const noexcept {
        const auto &p = r.plan;
        if (p.eqEnabled) {
            const auto change = equalizer.pendingLatencyChange();
            if (change.token != p.eqToken ||
                (change.token ? change.samples : equalizer.latencySamples()) != p.eqLatency)
                return false;
        }
        if (p.focusEnabled) {
            const auto *f = publishedFocus.load(std::memory_order_acquire);
            if (!f || f->configurationStatus().ready != p.focusReadyToken)
                return false;
            const auto change = f->pendingLatencyChange();
            if (change.token != r.focusOfferToken ||
                (change.token ? change.samples : f->latencySamples()) != p.focusLatency)
                return false;
        }
        return !p.carveEnabled || publishedCarve.load(std::memory_order_acquire) != nullptr;
    }
    BlockContext begin(std::uint64_t block, int configured, const Settings &s) noexcept {
        if (configured < 0 || !prepared || !supported(s))
            return {};
        // Mailbox contention delays configuration, never valid active audio.
        static_cast<void>(request(s));
        if (block && block == physical)
            return blockValid && configured == activePlan.totalLatency
                       ? BlockContext{epoch, generation, serial}
                       : BlockContext{};
        physical = block;
        ++serial;
        blockValid = false;
        Ready r;
        bool available = false;
        if (candidateVersion) {
            if (audioReady.plan.token == candidateVersion) {
                r = audioReady;
                available = true;
            } else if (ready(r) && r.plan.token == candidateVersion) {
                audioReady = r;
                available = true;
            }
        }
        if (available && (r.plan.carveEnabled || r.plan.focusEnabled)) {
            audioKeys = publishedKeys.load(std::memory_order_acquire);
            if (keySeenEpoch.load(std::memory_order_relaxed) != r.keyEpoch) {
                warmFrames.store(0, std::memory_order_release);
                keySeenEpoch.store(r.keyEpoch, std::memory_order_release);
            }
            primeVersion = candidateVersion;
        }
        if (available && r.primeEq && eqPrimedVersion.load(std::memory_order_relaxed) != candidateVersion) {
            eqWarmFrames.store(0, std::memory_order_release);
            eqPrimedVersion.store(candidateVersion, std::memory_order_release);
        }
        const bool admit = available && warm(r) && configured == r.plan.totalLatency;
        if ((!admit && configured != activePlan.totalLatency) || (admit && !validateReady(r)))
            return {};
        const auto &selected = admit ? candidate.settings : active;
        const auto &p = admit ? r.plan : activePlan;
        const auto &control = controls();
        eq::BlockContext nextEq{};
        focus::BlockContext nextFocus{};
        auto *selectedFocus = admit ? publishedFocus.load(std::memory_order_acquire) : audioFocus;
        if (!admit && available && r.primeEq) {
            eqPrimeContext = equalizer.beginBlock(block, r.plan.eqLatency, control.eq, false);
            if (!eqPrimeContext.generation)
                return {};
        }
        // With an immutable ready offer and validated child tokens these calls
        // cannot select different latency plans. Held callbacks explicitly deny
        // child admission, including offers whose latency happens to be equal.
        if (selected.eqEnabled) {
            nextEq = equalizer.beginBlock(block, p.eqLatency, control.eq, admit);
            if (!nextEq.generation)
                return {};
        }
        if (selected.focusEnabled) {
            nextFocus = selectedFocus->beginBlock(block, p.focusLatency, control.focus, admit);
            if (!nextFocus.epoch ||
                (admit && selectedFocus->configurationStatus().applied != p.focusReadyToken))
                return {};
        }
        eqContext = nextEq;
        focusContext = nextFocus;
        if (admit) {
            active = candidate.settings;
            activePlan = p;
            ++generation;
            audioCarve = publishedCarve.load(std::memory_order_acquire);
            audioFocus = selectedFocus;
            committedLatency.store(p.totalLatency, std::memory_order_release);
            if (!downstream(active)) {
                warmFrames.store(0, std::memory_order_release);
                primeVersion = 0;
            }
            candidateVersion = 0;
            // This release is the lifetime boundary: subsequent audio calls no
            // longer dereference children excluded by the newly admitted plan.
            acknowledged.store(p.token, std::memory_order_release);
        }
        blockValid = true;
        return {epoch, generation, serial};
    }
    template <class Sample>
    bool process(Sample *const *main, const Sample *const *key, int keyChannels, int frames,
                 const Settings &s, BlockContext context) noexcept {
        constexpr int samplePrecision = std::is_same_v<Sample, float> ? 1 : 2;
        if (!prepared || !blockValid || context.epoch != epoch || context.generation != generation ||
            context.block != serial || frames < 0 || frames > spec.maximumFrames || !main || !main[0] ||
            (spec.channels == 2 && !main[1]) || keyChannels < 0 || keyChannels > 2 || (keyChannels && !key) ||
            (keyChannels && keyLayout && keyChannels != keyLayout) ||
            (precision && precision != samplePrecision) || !supported(s))
            return false;
        static_cast<void>(request(s));
        if (!frames)
            return true;
        precision = samplePrecision;
        if (keyChannels)
            keyLayout = keyChannels;
        std::array<const Sample *, 2> rawKey{};
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const auto *zero = [&]() {
                if constexpr (std::is_same_v<Sample, float>)
                    return zeroFloat[ch].data();
                else
                    return zeroDouble[ch].data();
            }();
            rawKey[ch] = ch < static_cast<std::size_t>(keyChannels) && key[ch] ? key[ch] : zero;
        }
        const bool capture =
            audioKeys && (downstream(active) || (candidateVersion && primeVersion == candidateVersion));
        typename AlignedKey<Sample>::View aligned{};
        if (capture) {
            if constexpr (std::is_same_v<Sample, float>)
                aligned = audioKeys->single.capture(key, keyChannels, frames, activePlan.eqLatency,
                                                    downstream(active));
            else
                aligned = audioKeys->dual.capture(key, keyChannels, frames, activePlan.eqLatency,
                                                  downstream(active));
            if (!aligned.valid)
                return false;
            stats.keyFrames += static_cast<std::uint64_t>(frames);
            warmFrames.store(
                std::min(static_cast<std::uint64_t>(maximumEqLatency),
                         warmFrames.load(std::memory_order_relaxed) + static_cast<std::uint64_t>(frames)),
                std::memory_order_release);
        }
        const auto &control = controls();
        if (!active.eqEnabled && candidateVersion &&
            eqPrimedVersion.load(std::memory_order_relaxed) == candidateVersion) {
            std::array<Sample *, 2> scratch{};
            for (int ch = 0; ch < spec.channels; ++ch) {
                const auto index = static_cast<std::size_t>(ch);
                if constexpr (std::is_same_v<Sample, float>)
                    scratch[index] = scratchFloat[index].data();
                else
                    scratch[index] = scratchDouble[index].data();
                std::copy_n(main[ch], frames, scratch[index]);
            }
            const auto &e = control.eq;
            const int eqKeyChannels = keyChannels == 0 && e.externalSidechain ? 2 : keyChannels;
            if (!equalizer.process(scratch.data(), rawKey.data(), eqKeyChannels, frames, e, eqPrimeContext))
                return false;
            stats.eqPrimingFrames += static_cast<std::uint64_t>(frames);
            eqWarmFrames.store(
                std::min(static_cast<std::uint64_t>(maximumEqLatency),
                         eqWarmFrames.load(std::memory_order_relaxed) + static_cast<std::uint64_t>(frames)),
                std::memory_order_release);
        }
        if (active.eqEnabled) {
            const auto &e = control.eq;
            // An explicitly external detector with no route receives silence.
            // It must not accidentally become programme self-detection.
            const int eqKeyChannels = keyChannels == 0 && e.externalSidechain ? 2 : keyChannels;
            if (!equalizer.process(main, rawKey.data(), eqKeyChannels, frames, e, eqContext))
                return false;
            stats.eqFrames += static_cast<std::uint64_t>(frames);
        }
        const int alignedChannels = aligned.channels ? keyLayout : 0;
        if (active.carveEnabled) {
            const auto &c = control.carve;
            if (!audioCarve->process(main, aligned.samples, alignedChannels, frames, c))
                return false;
            stats.carveFrames += static_cast<std::uint64_t>(frames);
        }
        if (active.focusEnabled) {
            const auto &f = control.focus;
            if (!audioFocus->process(main, aligned.samples, alignedChannels, frames, f, focusContext))
                return false;
            stats.focusFrames += static_cast<std::uint64_t>(frames);
        }
        return true;
    }
};

Core::Core() : impl_(std::make_unique<Impl>()) {}
Core::~Core() = default;
void Core::prepare(PrepareSpec spec, const Settings &s) {
    auto next = std::make_unique<Impl>();
    next->prepare(spec, s);
    impl_.swap(next);
}
void Core::reset(const Settings &s) { prepare(impl_->spec, s); }
bool Core::requestConfiguration(const Settings &s) noexcept { return impl_->request(s); }
bool Core::serviceConfiguration() { return impl_->service(); }
PlanOffer Core::pendingPlan() const noexcept { return impl_->pending(); }
std::int32_t Core::latencySamples() const noexcept {
    return impl_->committedLatency.load(std::memory_order_acquire);
}
BlockContext Core::beginBlock(std::uint64_t b, std::int32_t l, const Settings &s) noexcept {
    return impl_->begin(b, l, s);
}
bool Core::process(float *const *m, const float *const *k, std::int32_t kc, std::int32_t n, const Settings &s,
                   BlockContext b) noexcept {
    return impl_->process(m, k, kc, n, s, b);
}
bool Core::process(double *const *m, const double *const *k, std::int32_t kc, std::int32_t n,
                   const Settings &s, BlockContext b) noexcept {
    return impl_->process(m, k, kc, n, s, b);
}
Statistics Core::statistics() const noexcept {
    auto s = impl_->stats;
    s.focusPreparations = impl_->focusPreparations.load(std::memory_order_acquire);
    s.eqResets = impl_->eqResets.load(std::memory_order_acquire);
    s.focusPrepared = impl_->publishedFocus.load(std::memory_order_acquire) != nullptr;
    s.keyPrepared = impl_->publishedKeys.load(std::memory_order_acquire) != nullptr;
    return s;
}
} // namespace adi::mix::eq_chain
