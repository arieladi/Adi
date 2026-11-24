// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// adi_play-only measurement storage. Allocate before starting audio; consume
// only after the callback has been removed. No runtime engine policy lives here.
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <span>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace adi_play {
inline std::int64_t measurementTicks() noexcept {return juce::Time::getHighResolutionTicks();}
inline std::uint32_t measurementThread() noexcept {
#if defined(_WIN32)
    return GetCurrentThreadId();
#else
    return 0;
#endif
}
inline std::uint32_t measurementCpu() noexcept {
#if defined(_WIN32)
    return GetCurrentProcessorNumber();
#else
    return UINT32_MAX;
#endif
}
inline std::uint32_t measurementPid() noexcept {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}
struct CallbackTiming {
    std::int64_t startTicks=0,endTicks=0,streamSample=0;
    double sampleRate=0;
    std::int32_t frames=0,numIn=0,numOut=0;
    std::uint32_t threadId=0,cpuStart=UINT32_MAX,cpuEnd=UINT32_MAX;
};
class CallbackTimings {
public:
    explicit CallbackTimings(std::size_t capacity):rows_(capacity){}
    // Single callback writer. No allocations, locks, output or shared refcounts.
    void record(const CallbackTiming& row) noexcept {
        if(written_<rows_.size())rows_[written_++]=row;else ++dropped_;
        completed_.fetch_add(1,std::memory_order_release);
    }
    [[nodiscard]] std::uint64_t observed()const noexcept{return completed_.load(std::memory_order_acquire);}
    // Owner only after the device callback has stopped and been removed.
    [[nodiscard]] std::span<const CallbackTiming> rows()const noexcept{return {rows_.data(),written_};}
    [[nodiscard]] std::uint64_t dropped()const noexcept{return dropped_;}
    [[nodiscard]] std::size_t capacity()const noexcept{return rows_.size();}
private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::vector<CallbackTiming> rows_;
    std::size_t written_=0;
    std::uint64_t dropped_=0;
    std::atomic<std::uint64_t> completed_{0};
};
inline std::filesystem::path diagnosticPath(const std::string& prefix,const char* suffix) {
    const auto bytes=prefix+suffix;return std::filesystem::path(std::u8string(bytes.begin(),bytes.end()));
}
inline nlohmann::json timingSummary(std::span<const CallbackTiming> rows,std::int64_t minimumTick,std::int64_t frequency) {
    std::vector<double> micros;std::uint64_t overBudget=0;std::int64_t frames=0;
    for(const auto& row:rows)if(row.startTicks>=minimumTick && row.endTicks>=row.startTicks) {
        const double seconds=static_cast<double>(row.endTicks-row.startTicks)/static_cast<double>(frequency);
        micros.push_back(seconds*1e6);frames+=row.frames;
        if(row.sampleRate>0 && seconds>static_cast<double>(row.frames)/row.sampleRate)++overBudget;
    }
    std::sort(micros.begin(),micros.end());
    const auto percentile=[&](double p)->nlohmann::json {
        if(micros.empty())return nullptr;
        const auto index=static_cast<std::size_t>(std::ceil(p*static_cast<double>(micros.size())))-1;
        return micros[std::min(index,micros.size()-1)];
    };
    return {{"callbacks",micros.size()},{"frames",frames},{"p50_us",percentile(0.50)},
        {"p99_us",percentile(0.99)},{"max_us",micros.empty()?nlohmann::json(nullptr):nlohmann::json(micros.back())},
        {"processing_over_buffer_duration",overBudget},{"quantile_method","nearest rank"}};
}
inline bool writeDiagnostics(const std::string& prefix,const CallbackTimings& timings,
    nlohmann::json receipt,std::int64_t readyTicks,std::string& error) {
    try {
        const auto frequency=juce::Time::getHighResolutionTicksPerSecond();
        receipt["clock"]={{"name",
#if defined(_WIN32)
            "QueryPerformanceCounter"
#else
            "JUCE high-resolution ticks"
#endif
        },{"ticks_per_second",frequency}};
        receipt["capture"]={{"capacity",timings.capacity()},{"stored",timings.rows().size()},
            {"observed",timings.observed()},{"dropped",timings.dropped()},
            {"scope","DeviceBridge callback including Session and adi_play peak meter; excludes outer JUCE/ASIO and trace-row write"}};
        receipt["all_callbacks"]=timingSummary(timings.rows(),0,frequency);
        receipt["after_ready"]=timingSummary(timings.rows(),readyTicks,frequency);
        receipt["limitations"]={"processing over budget is not an observed physical dropout",
            "MMCSS category and priority inheritance not verified; no worker initializer supplied",
            "CPU numbers identify logical processors, not affinity or residency between samples",
            "graph counters describe the final published generation, not a sum over retired generations"};
        std::ofstream csv(diagnosticPath(prefix,".callbacks.csv"),std::ios::binary|std::ios::trunc);
        if(!csv)throw std::runtime_error("cannot create callback CSV");
        csv<<"sequence,start_ticks,end_ticks,duration_ticks,frames,num_in,num_out,stream_sample,sample_rate,callback_thread_id,cpu_start,cpu_end\n";
        csv<<std::setprecision(17);std::uint64_t sequence=0;
        for(const auto& row:timings.rows())csv<<sequence++<<','<<row.startTicks<<','<<row.endTicks<<','<<row.endTicks-row.startTicks<<','
            <<row.frames<<','<<row.numIn<<','<<row.numOut<<','<<row.streamSample<<','<<row.sampleRate<<','
            <<row.threadId<<','<<row.cpuStart<<','<<row.cpuEnd<<'\n';
        csv.close();if(!csv)throw std::runtime_error("cannot finish callback CSV");
        std::ofstream json(diagnosticPath(prefix,".json"),std::ios::binary|std::ios::trunc);
        if(!json)throw std::runtime_error("cannot create diagnostic JSON");
        json<<receipt.dump(2)<<'\n';json.close();if(!json)throw std::runtime_error("cannot finish diagnostic JSON");
        return true;
    }catch(const std::exception& failure){error=failure.what();return false;}
}
} // namespace adi_play
