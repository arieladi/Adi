// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/render_jobs.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace {
using namespace adi;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void run() {
    test::TempDirectory temp("tracked_render", "owner");
    StoreError error{};
    auto store = Store::createEmpty(temp.path() / "project.adi", error);
    require(static_cast<bool>(store), "open the render fixture");
    store->db().exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Old',48000);"
                     "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
                     "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(9,'master','Main',0);");
    const auto output = temp.path() / "out";
    std::filesystem::create_directory(output);
    const auto job = [&](const char* name) {
        render::Job value;
        value.project = store->path();
        value.directory = output;
        value.nameTemplate = name;
        value.endSample = 1024;
        value.session = {2, 48000, 64};
        return value;
    };

    control::JobBook book;
    control::TrackedRenderQueue queue(book);
    const auto project = store->projectUuid();
    const auto pending = queue.enqueue("alice", project, job("pending"));
    const auto running = queue.enqueue("alice", project, job("running"));
    const auto other = queue.enqueue("bob", project, job("{project}-completed"));
    require(pending && running && other, "the renderer registers jobs before run");
    require(!book.cancel("bob", pending->id), "another client cannot cancel the pending job");
    require(book.cancel("alice", pending->id).has_value(), "the owner cancels while pending");

    OpRequest rename;
    rename.opType = "project.setName";
    rename.payload = {{"name", "At start"}};
    const auto moved = OpJournal(*store).commit(rename);
    require(moved.ok && !moved.seqs.empty(), "move the project past its enqueue revision");
    const auto runningId = queue.snapshot().at(1).id;
    bool sawRunning = false;
    bool cancelled = false;
    render::Control renderControl;
    renderControl.progress = [&](render::JobId id, std::uint64_t written, std::uint64_t) {
        if (id != runningId || cancelled || written == 0) return;
        const auto current = book.get("alice", running->id);
        sawRunning = current && current->state == control::JobState::Running && current->progress > 0.0;
        cancelled = book.cancel("alice", running->id).has_value();
    };
    queue.run({}, std::move(renderControl));
    require(sawRunning && cancelled, "running jobs expose progress and accept owner cancellation");
    const auto first = book.get("alice", pending->id);
    const auto second = book.get("alice", running->id);
    const auto third = book.get("bob", other->id);
    require(first && first->state == control::JobState::Cancelled &&
            second && second->state == control::JobState::Cancelled, "both cancellations reach terminal state");
    require(third && third->state == control::JobState::Completed, "one member's cancellation leaves the next render running");
    const auto& report = queue.lastRun();
    require(report.plannedCount == 3 && report.members.at(0).removedBeforeStart,
            "run membership was fixed before skipping the cancelled job");
    const auto& rendered = report.members.at(2).result;
    require(rendered && rendered->snapshot && rendered->snapshot->journalHead == moved.seqs.back() &&
            rendered->snapshot->names.project == "At start", "the render captured the revision and names at its start");
    require(std::filesystem::exists(output / "At start-completed.wav") &&
            !std::filesystem::exists(output / "pending.wav") && !std::filesystem::exists(output / "running.wav"),
            "only the completed member published a file");
}
}  // namespace

int main() {
    try {
        run();
        std::puts("PASS -- tracked render ownership, cancellation and start revision");
        return 0;
    } catch (const std::exception& exception) {
        std::printf("FAIL -- %s\n", exception.what());
        return 1;
    }
}
