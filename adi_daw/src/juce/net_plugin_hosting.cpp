// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/net_plugin_hosting.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/control/consent_scope.hpp"
#include "adi/store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace adi::net {
namespace {

constexpr auto kForever = std::numeric_limits<std::int64_t>::max();
constexpr std::size_t kMaxQuestions = 32;

std::string endpointText(const PluginHostingScope& scope) {
    const auto& host = scope.address.host;
    const bool literalV6 = host.find(':') != std::string::npos;
    return "tcp://" + (literalV6 ? "[" + host + "]" : host) + ":" + std::to_string(scope.address.port) + "/" +
           scope.serverFingerprint;
}

bool sameRequest(const control::ConsentRequest& a, const control::ConsentRequest& b) {
    return a.projectUuid == b.projectUuid && a.destination.connection == b.destination.connection &&
           a.destination.endpoint == b.destination.endpoint && a.fileBinding == b.fileBinding &&
           a.data.size() == b.data.size() &&
           std::equal(a.data.begin(), a.data.end(), b.data.begin(), [](const auto& x, const auto& y) {
               return x.kind == y.kind && x.bytes == y.bytes;
           });
}

} // namespace

std::int64_t systemUnixSeconds() {
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(since).count());
}

control::ConsentRequest pluginHostingRequest(const PluginHostingScope& scope, std::string fileBinding) {
    using control::ConsentData;
    control::ConsentRequest request;
    request.projectUuid = scope.projectUuid;
    request.destination.connection = scope.connectionId;
    request.destination.endpoint = endpointText(scope);
    request.capability = control::ConsentCapability::PluginHosting;
    request.data.push_back({ConsentData::Audio, scope.audioBytesPerBlock});
    request.data.push_back({ConsentData::Notes, PluginHostingDisclosure::eventBytesPerBlock});
    request.data.push_back({ConsentData::Parameters, PluginHostingDisclosure::parameterBytes});
    request.data.push_back({ConsentData::PluginState, PluginHostingDisclosure::stateBytes});
    request.fileBinding = std::move(fileBinding);
    return request;
}

struct PluginHostingLeases::Shared {
    struct Lease {
        control::ConsentRequest request;
        bool standing = false;
        std::atomic<bool> ended{false};
        std::atomic<std::int64_t> until{kForever};
    };

    Shared(control::ConsentLedger file, UnixSecondsClock clock) : ledger(std::move(file)), now(std::move(clock)) {}

    // Callers hold the mutex.
    template <class Visit>
    void eachLive(Visit visit) {
        std::erase_if(leases, [](const std::weak_ptr<Lease>& held) { return held.expired(); });
        for (const auto& held : leases) {
            const auto lease = held.lock();
            if (lease && !lease->ended.load(std::memory_order_acquire)) visit(*lease);
        }
    }

    void ask(const PluginHostingScope& scope, const control::ConsentRequest& request) {
        const std::lock_guard lock(mutex);
        if (questions.size() >= kMaxQuestions ||
            std::ranges::any_of(questions, [&](const auto& asked) { return sameRequest(asked.request, request); }))
            return;
        questions.push_back({nextQuestion++, scope, request});
    }

    control::ConsentLedger ledger;
    UnixSecondsClock now;
    std::mutex mutex;
    std::vector<std::weak_ptr<Lease>> leases;
    std::vector<PluginHostingQuestion> questions;
    std::uint64_t nextQuestion = 1;
};

PluginHostingLeases::PluginHostingLeases(control::ConsentLedger ledger, UnixSecondsClock now)
    : shared_(std::make_shared<Shared>(std::move(ledger), now ? std::move(now) : UnixSecondsClock(systemUnixSeconds))) {}

PluginHostingAuthorize PluginHostingLeases::authorizer() const {
    return [shared = shared_](const Store& store, const PluginHostingScope& scope) -> PluginHostingLease {
        if (audio::onAudioThread) return {};
        auto request = pluginHostingRequest(scope, control::projectFileConsentBinding(store));
        const auto now = shared->now();
        // Read the standing grants first: authorize consumes a This-time grant.
        const auto standing = shared->ledger.standing(request, now);
        if (standing.decision == control::ConsentDecision::Unavailable) return {};
        const auto decision = shared->ledger.authorize(request, now);
        if (decision == control::ConsentDecision::ConsentRequired) shared->ask(scope, request);
        if (decision != control::ConsentDecision::Allowed) return {};

        auto lease = std::make_shared<Shared::Lease>();
        lease->request = std::move(request);
        lease->standing = standing.decision == control::ConsentDecision::Allowed;
        lease->until.store(lease->standing ? standing.until : kForever, std::memory_order_release);
        {
            const std::lock_guard lock(shared->mutex);
            shared->eachLive([](const Shared::Lease&) {});
            shared->leases.push_back(lease);
        }
        // Checked on the device's I/O worker: two atomic loads and the clock.
        return [lease, clock = shared->now] {
            return !lease->ended.load(std::memory_order_acquire) &&
                   clock() < lease->until.load(std::memory_order_acquire);
        };
    };
}

std::vector<PluginHostingQuestion> PluginHostingLeases::takeQuestions() {
    const std::lock_guard lock(shared_->mutex);
    std::vector<PluginHostingQuestion> taken;
    taken.swap(shared_->questions);
    return taken;
}

bool PluginHostingLeases::answer(const PluginHostingQuestion& question, control::ConsentTerm term) {
    return !audio::onAudioThread && shared_->ledger.grant(question.request, term, shared_->now());
}

std::size_t PluginHostingLeases::revoke(std::string_view projectUuid, std::string_view connectionId) {
    if (audio::onAudioThread) return 0;
    const std::lock_guard lock(shared_->mutex);
    std::size_t ended = 0;
    shared_->eachLive([&](Shared::Lease& lease) {
        if (lease.request.projectUuid != projectUuid || lease.request.destination.connection != connectionId) return;
        // The lease ends whether or not the ledger could be written: revocation fails closed.
        lease.ended.store(true, std::memory_order_release);
        ++ended;
        if (lease.standing) (void)shared_->ledger.revoke(lease.request);
    });
    return ended;
}

std::size_t PluginHostingLeases::refresh() {
    if (audio::onAudioThread) return 0;
    const std::lock_guard lock(shared_->mutex);
    const auto now = shared_->now();
    std::size_t ended = 0;
    shared_->eachLive([&](Shared::Lease& lease) {
        if (!lease.standing) return;
        const auto standing = shared_->ledger.standing(lease.request, now);
        if (standing.decision == control::ConsentDecision::Allowed) {
            lease.until.store(standing.until, std::memory_order_release);
        } else if (standing.decision == control::ConsentDecision::ConsentRequired) {
            lease.ended.store(true, std::memory_order_release);
            ++ended;
        }
    });
    return ended;
}

std::size_t PluginHostingLeases::live() const {
    const std::lock_guard lock(shared_->mutex);
    const auto now = shared_->now();
    std::size_t count = 0;
    shared_->eachLive([&](const Shared::Lease& lease) {
        if (now < lease.until.load(std::memory_order_acquire)) ++count;
    });
    return count;
}

} // namespace adi::net
