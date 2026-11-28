// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct WalObservation {
    int threshold = 0;
    int frames = 0;
    bool checkpoint = false;
    int checkpointResult = SQLITE_OK;
    int checkpointedFrames = 0;

    static int afterCommit(void* user, sqlite3* db, const char* name, int frameCount) noexcept {
        auto& self = *static_cast<WalObservation*>(user);
        self.frames = frameCount;
        self.checkpoint = frameCount >= self.threshold;
        // Match the bundled SQLite's normal auto-checkpoint hook, including
        // PASSIVE mode and the separation of checkpoint and commit outcomes.
        if (self.checkpoint)
            self.checkpointResult = sqlite3_wal_checkpoint_v2(db, name, SQLITE_CHECKPOINT_PASSIVE,
                                                            nullptr, &self.checkpointedFrames);
        return SQLITE_OK;
    }
};

struct Row {
    const char* phase;
    int edit;
    int framesBefore;
    int framesAtCommit;
    bool checkpoint;
    int checkpointedFrames;
    double microseconds;
};

std::unique_ptr<adi::Store> newProject(const fs::path& path) {
    adi::StoreError error{};
    auto store = adi::Store::createNewProject(path, error);
    if (!store) throw std::runtime_error(std::string("could not create project: ") + adi::toString(error));
    return store;
}

std::int64_t audioTrack(adi::Store& store) {
    return store.db().execAndGet("SELECT id FROM tracks WHERE kind='audio' ORDER BY id LIMIT 1").getInt64();
}

adi::OpRequest renameTrack(std::int64_t track, const std::string& name) {
    adi::OpRequest request;
    request.opType = "track.rename";
    request.payload = {{"id", track}, {"name", name}};
    return request;
}

void measure(const fs::path& path, int repeat, bool full, bool print) {
    WalObservation wal;
    auto store = newProject(path);
    auto& db = store->db();
    require(db.execAndGet("PRAGMA journal_mode").getString() == "wal", "expected a WAL project");
    const int pageBytes = db.execAndGet("PRAGMA page_size").getInt();
    wal.threshold = db.execAndGet("PRAGMA wal_autocheckpoint").getInt();
    require(wal.threshold >= 500, "expected an enabled auto-checkpoint of at least 500 frames");
    db.exec(full ? "PRAGMA synchronous=FULL" : "PRAGMA synchronous=NORMAL");
    require(db.execAndGet("PRAGMA synchronous").getInt() == (full ? 2 : 1), "unexpected synchronous mode");
    require(sqlite3_wal_checkpoint_v2(db.getHandle(), "main", SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr)
                == SQLITE_OK, "could not start with an empty WAL");
    sqlite3_wal_hook(db.getHandle(), WalObservation::afterCommit, &wal);

    const auto track = audioTrack(*store);
    adi::OpJournal journal(*store);
    int edit = 0;
    auto commit = [&](const char* phase) {
        const auto request = renameTrack(track, "WAL rename " + std::to_string(++edit));
        const int before = wal.frames;
        const auto start = Clock::now();
        const auto result = journal.commit(request);
        const auto end = Clock::now();
        if (!result.ok) throw std::runtime_error("commit failed: " + result.error);
        require(result.seqs.size() == 1 && wal.frames > before, "expected one durable edit and new WAL frames");
        require(!wal.checkpoint || (wal.checkpointResult == SQLITE_OK && wal.checkpointedFrames == wal.frames),
                "the threshold PASSIVE checkpoint did not complete");
        return Row{phase, edit, before, wal.frames, wal.checkpoint, wal.checkpointedFrames,
                   std::chrono::duration<double, std::micro>(end - start).count()};
    };

    std::vector<Row> rows;
    rows.reserve(256);
    for (int i = 0; i < 12; ++i) (void)commit("warmup");
    for (int i = 0; i < 48; ++i) rows.push_back(commit("steady"));
    const int nearFull = wal.threshold * 9 / 10;
    require(wal.frames < nearFull && !wal.checkpoint, "steady sample reached near-full WAL");
    while (wal.frames < nearFull) (void)commit("fill");
    require(!wal.checkpoint, "fill skipped the measured near-full window");
    while (!wal.checkpoint) rows.push_back(commit("near_full"));

    require(db.execAndGet("SELECT count(*) FROM ops").getInt() == edit, "wrong durable edit count");
    require(db.execAndGet("PRAGMA quick_check").getString() == "ok", "project failed SQLite quick_check");
    require(store->close(), "could not close the measured project");
    if (print) {
        for (const auto& row : rows) {
            std::printf("%d,%s,%s,%d,%d,%d,%d,%d,%d,%d,%.3f\n", repeat, full ? "FULL" : "NORMAL",
                        row.phase, row.edit, pageBytes, wal.threshold, row.framesBefore, row.framesAtCommit,
                        row.checkpoint ? 1 : 0, row.checkpointedFrames, row.microseconds);
        }
    }
}

void testPinnedReader(const fs::path& path) {
    auto writer = newProject(path);
    auto& db = writer->db();
    const auto track = audioTrack(*writer);
    const std::string nameQuery = "SELECT name FROM tracks WHERE id=" + std::to_string(track);
    require(adi::OpJournal(*writer).commit(renameTrack(track, "Before reader")).ok,
            "could not seed the reader's view");
    adi::StoreError error{};
    auto reader = adi::Store::open(path, error, true);
    require(reader && reader->readOnly(), "could not open the read-only project");
    reader->db().exec("BEGIN");
    require(reader->db().execAndGet(nameQuery).getString() == "Before reader", "reader did not pin the expected rows");
    require(adi::OpJournal(*writer).commit(renameTrack(track, "After reader")).ok, "writer commit failed");

    int frames = -1, written = -1;
    const auto pass = [&] {
        return sqlite3_wal_checkpoint_v2(db.getHandle(), "main", SQLITE_CHECKPOINT_PASSIVE, &frames, &written);
    };
    require(pass() == SQLITE_OK && written > 0 && written < frames,
            "pinned reader must give a partial PASSIVE result with SQLITE_OK");
    std::fprintf(stderr, "PASS: pinned PASSIVE result %d/%d frames, SQLITE_OK\n", written, frames);
    require(reader->db().execAndGet(nameQuery).getString() == "Before reader", "pinned reader changed view");
    reader->db().exec("COMMIT");
    require(pass() == SQLITE_OK && written == frames && frames > 0,
            "releasing the reader must allow the rest to checkpoint");
    require(reader->db().execAndGet(nameQuery).getString() == "After reader", "released reader did not follow writer");
    reader.reset();
    require(writer->close(), "could not close the reader test project");
}

int repeatCount(std::string_view argument) {
    int value = 0;
    const auto end = argument.data() + argument.size();
    const auto decoded = std::from_chars(argument.data(), end, value);
    require(decoded.ec == std::errc{} && decoded.ptr == end && value >= 1 && value <= 30,
            "repeat count must be an integer from 1 to 30");
    return value;
}
} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        if (argc == 2 && fs::path(argv[1]) == "--self-test") {
            adi::test::TempDirectory temp{"project_wal", "self_test"};
            testPinnedReader(temp.path() / "reader.adi");
            measure(temp.path() / "normal.adi", 1, false, false);
            measure(temp.path() / "full.adi", 1, true, false);
            std::puts("PASS: NORMAL and FULL project commit cohorts cross the checkpoint threshold");
            return 0;
        }
        if (argc != 3) {
            std::fputs("Usage: adi_project_wal_benchmark --self-test | NEW_PROJECTS_DIRECTORY REPEATS\n", stderr);
            return 2;
        }
        const int repeats = repeatCount(fs::path(argv[2]).string());
        const fs::path directory(argv[1]);
        require(fs::create_directory(directory), "projects directory must not exist");
        std::fprintf(stderr, "SQLite %s, %d paired repeats\n", sqlite3_libversion(), repeats);
        testPinnedReader(directory / "reader.adi");
        std::puts("repeat,mode,phase,edit,page_bytes,threshold,wal_frames_before,wal_frames_at_commit,checkpoint,checkpointed_frames,commit_us");
        for (int repeat = 1; repeat <= repeats; ++repeat) {
            const auto name = std::to_string(repeat);
            const auto normal = [&] { measure(directory / (name + "-normal.adi"), repeat, false, true); };
            const auto full = [&] { measure(directory / (name + "-full.adi"), repeat, true, true); };
            if (repeat % 2 == 1) { normal(); full(); }
            else { full(); normal(); }
        }
        return std::ferror(stdout) == 0 && std::fflush(stdout) == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Project WAL benchmark: %s\n", e.what());
        return 1;
    }
}
