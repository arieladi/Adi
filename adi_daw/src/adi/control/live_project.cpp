// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/live_project.hpp"

#include "adi/control/headless_project.hpp"
#include "adi/ui/project_document.hpp"

#include <utility>

namespace adi::control {
namespace {
constexpr const char* kClosed = "the project is no longer open at this location";
}

LiveProject::LiveProject(ui::ProjectDocument& document, std::shared_ptr<HeadlessProject> reader)
    : path_(document.store().path()), reader_(std::move(reader)),
      writable_(!document.store().readOnly()), document_(&document) {}

std::shared_ptr<LiveProject> LiveProject::open(ui::ProjectDocument& document, std::string& error) {
    auto reader = HeadlessProject::open(document.store().path(), error);
    if (!reader) return nullptr;
    if (reader->projectUuid() != document.store().projectUuid()) {
        error = "the project reader found a different project";
        return nullptr;
    }
    return std::shared_ptr<LiveProject>(new LiveProject(document, std::move(reader)));
}

ui::ProjectDocument* LiveProject::ownerDocument() const {
    return document_ && document_->store().path() == path_ ? document_ : nullptr;
}

bool LiveProject::matches(ui::ProjectDocument& document) const {
    return ownerDocument() == &document;
}

void LiveProject::detach() {
    attached_.store(false, std::memory_order_release);
    document_ = nullptr;
    board_.publish(nullptr);
}

const std::string& LiveProject::handle() const { return reader_->handle(); }
std::string LiveProject::projectUuid() const { return reader_->projectUuid(); }
std::string LiveProject::label() const { return live() ? reader_->label() : "Closed project"; }

ReadSnapshot LiveProject::read(std::string& error) {
    if (live()) return reader_->read(error);
    error = kClosed;
    return {};
}

std::optional<std::int64_t> LiveProject::head(std::string& error) {
    if (live()) return reader_->head(error);
    error = kClosed;
    return std::nullopt;
}

std::string LiveProject::fileSchema() { return live() ? reader_->fileSchema() : std::string{}; }

agent::Preview LiveProject::preview(const agent::Changeset& changeset, const agent::Limits& limits) {
    if (auto* document = ownerDocument()) return agent::preview(document->store(), changeset, limits);
    agent::Preview result;
    result.refusal = agent::Refusal::ReadOnly;
    result.error = kClosed;
    return result;
}

std::optional<Tier> LiveProject::agentTierOverride() {
    return live() ? reader_->agentTierOverride() : std::optional<Tier>{Tier::Observe};
}

bool LiveProject::allowsAutomaticApply() const { return live() && writable_; }

agent::ApplyResult LiveProject::applyChangeset(const agent::Changeset& changeset, const agent::Limits& limits) {
    auto* document = ownerDocument();
    if (!document) return {false, agent::Refusal::ReadOnly, kClosed};

    ui::OpSubmitter submitter(document->store(), document->view(), Actor::Agent, changeset.actorDetail);
    submitter.followOverrides(document->session().overriddenLanes());
    agent::ApplyResult applied;
    try {
        (void)submitter.commitPrepared([&](Store& store, Actor) {
            applied = agent::apply(store, changeset, limits);
            CommitResult journal;
            journal.ok = applied.ok;
            journal.txnId = applied.txnId;
            journal.seqs = applied.seqs;
            return journal;
        });
    } catch (...) {
        // A failed view refresh cannot turn a durable commit into a refusal.
        if (!applied.ok) throw;
    }
    return applied;
}

bool LiveProject::wasApplied(const std::string& actorDetail) {
    return ownerDocument() && reader_->wasApplied(actorDetail);
}

bool LiveProject::live() const { return attached_.load(std::memory_order_acquire); }

std::shared_ptr<const TelemetryFrame> LiveProject::telemetry() const {
    return live() ? board_.latest() : nullptr;
}

std::string LiveProject::applyHint() const { return "Review this changeset in ADI."; }

void LiveProject::publishTelemetry(std::int64_t nowMs) {
    auto* document = ownerDocument();
    if (!document) return;
    const auto& session = document->session();
    auto& transport = document->mailbox();
    MeterTaps taps;
    if (const auto snapshot = document->view().committed()) {
        taps.strips.reserve(snapshot->tracks.size());
        for (const auto& track : snapshot->tracks)
            if (const auto* strip = session.strips().stripFor(track->id))
                taps.strips.emplace_back(track->id, &strip->meter());
    }
    taps.main = &session.mainLoudness().readout();
    taps.transport = [&transport] {
        return TransportReading{transport.playing(), transport.position(), transport.sampleRate()};
    };
    board_.publish(sample(taps, ++sequence_, nowMs));
}

}  // namespace adi::control
