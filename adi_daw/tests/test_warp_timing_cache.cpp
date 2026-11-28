// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_timing_cache.hpp"
#include "temp_directory.hpp"
#include <cstdio>

namespace {
using namespace adi;
using P = media::AnalysisCacheProblem;
int errors = 0;
void check(bool ok, const char* label) {
    if (!ok) { ++errors; std::printf("FAIL %s\n", label); }
}
}

int main() {
    test::TempDirectory temp{"warp_timing", "cache"};
    media::AnalysisCache cache(temp.path() / "analysed");
    media::AnalysisIdentity identity{std::string(64, 'a'), 96000, 2, 480000};
    audio::WarpTimingSettings settings{4, 35};
    audio::WarpTimingAnalysis grid;
    grid.onsets = {{16534, 0.2f, -8.0f}, {33075, 0.9f, -4.5f}};
    grid.beats = {11025, 33075, 55125, 77175, 99225, 116865, 134505};
    grid.downbeats = {1, 5};
    grid.tempoRegions = {{0, 120}, {4, 150}};
    grid.medianBpm = 120;
    std::string error;
    check(cache.update(identity, "TEST", {1, 0, {std::byte{0x7f}}}, error) == P::None,
          "create an unrelated section");
    check(!audio::readWarpTiming(cache, identity, settings), "missing timing section is a miss");

    check(audio::storeWarpTiming(cache, identity, settings, grid, error) == P::None,
          "write the derived timing section");
    const auto result = audio::readWarpTiming(cache, identity, settings);
    check(result && result->onsets == grid.onsets && result->beats == grid.beats &&
          result->downbeats == grid.downbeats && result->tempoRegions.size() == 2 &&
          result->tempoRegions.back().firstBeat == 4 && result->tempoRegions.back().bpm == 150 &&
          result->medianBpm == 120, "read back onsets, peaks, beat and bar clocks, regions and tempo exactly");
    const auto record = cache.read(identity);
    check(record && record.data->sections.contains("TEST"), "timing publication preserves other sections");
    check(!audio::readWarpTiming(cache, identity, {4, 36}) &&
          !audio::readWarpTiming(cache, identity, {3, 35}), "sensitivity and meter both invalidate the cached calculation");

    auto invalid = grid;
    invalid.tempoRegions.back().firstBeat = invalid.beats.size() - 1;
    check(audio::storeWarpTiming(cache, identity, settings, invalid, error) == P::Invalid,
          "a tempo region requires a complete beat interval");
    invalid = grid;
    invalid.onsets.front().strength = 1.1f;
    check(audio::storeWarpTiming(cache, identity, settings, invalid, error) == P::Invalid,
          "onset strengths must stay within their normalized range");
    std::stop_source stopped;
    stopped.request_stop();
    check(audio::storeWarpTiming(cache, identity, settings, grid, error, stopped.get_token()) == P::Cancelled,
          "cancelled timing publication leaves the stored result intact");
    {
        audio::CallbackScope callback;
        check(!audio::readWarpTiming(cache, identity, settings) &&
              audio::storeWarpTiming(cache, identity, settings, grid, error) == P::Realtime,
              "audio callback refuses timing-cache work");
    }

    if (record) {
        auto corrupt = record.data->sections.at("AWTM");
        corrupt.bytes.pop_back();
        check(cache.update(identity, "AWTM", corrupt, error) == P::None &&
              !audio::readWarpTiming(cache, identity, settings),
              "a truncated timing payload inside a valid ADIA envelope is a miss");
    }
    return errors ? 1 : 0;
}
