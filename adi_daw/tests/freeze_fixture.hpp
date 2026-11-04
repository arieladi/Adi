#pragma once
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/render/freeze_capture.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <stdexcept>
namespace {
using namespace adi;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
struct Counts {
    int loads = 0;
    std::array<std::uint64_t, 3> frames{};
};
struct Delayed : device::DeviceInstance {
    device::DeviceIdentity identity_;
    Counts *counts = nullptr;
    std::size_t index = 0;
    std::int32_t delay = 0, at = 0;
    std::array<std::array<double, 128>, 2> ring{};
    bool consume = false, poison = false, change = false;
    std::uint64_t epoch = 0;
    const device::DeviceIdentity &identity() const noexcept override {
        return identity_;
    }
    bool loaded() const noexcept override {
        return true;
    }
    bool processesDouble() const noexcept override {
        return true;
    }
    engine::EventFlow eventFlow() const noexcept override {
        return consume ? engine::EventFlow::Consume : engine::EventFlow::Through;
    }
    bool loadState(const std::string &, const std::vector<std::uint8_t> &bytes) override {
        consume = !bytes.empty() && bytes[0] == 1;
        return true;
    }
    std::int32_t latencySamples() const noexcept override {
        return delay;
    }
    std::uint64_t latencyEpoch() const noexcept override {
        return epoch;
    }
    void process(const engine::NodeIo &io) noexcept override {
        for (int f = 0; f < io.frames; ++f) {
            for (int c = 0; c < io.channels; ++c) {
                const auto channel = static_cast<std::size_t>(c),
                           sample = static_cast<std::size_t>(io.blockOffset + f);
                const double input = io.wide() ? io.in64[c][sample] : io.in[c][sample];
                const auto read = static_cast<std::size_t>((at + 128 - delay) % 128);
                const double value = poison ? std::numeric_limits<double>::quiet_NaN()
                                            : ring[channel][read] * (index == 2 ? 0.25 : 1.0);
                ring[channel][static_cast<std::size_t>(at)] = input;
                if (io.wide())
                    io.out64[c][sample] = value;
                else
                    io.out[c][sample] = static_cast<float>(value);
            }
            at = (at + 1) % 128;
        }
        counts->frames[index] += static_cast<std::uint64_t>(io.frames);
        if (change && counts->frames[index] >= 128 && epoch == 0) {
            ++delay;
            ++epoch;
        }
    }
};
float signal(std::uint64_t frame, unsigned channel) {
    return static_cast<float>(static_cast<int>((frame * 2 + channel) % 59) - 29) / 64;
}
struct Project {
    test::TempDirectory root{"freeze_capture", "project"};
    std::unique_ptr<Store> store;
    Project() {
        StoreError error{};
        store = Store::create(root.path() / "source.adi", error);
        if (!store)
            throw std::runtime_error(toString(error));
        store->db().exec(
            "INSERT INTO tracks(id,kind,name,index_in_parent) "
            "VALUES(1,'midi','Chain',0),(2,'audio','Plain',1),(9,'master','Main',2);INSERT INTO "
            "plugin_refs(id,format,uid,name,version,subtype) VALUES(1,'vst3','test.midifx','MIDI "
            "FX','stored-version','effect'),(2,'vst3','test.synth','Synth','stored-version','effect'),(3,'"
            "vst3','test.audiofx','Audio FX','stored-version','effect');INSERT INTO "
            "device_chains(id,track_id) VALUES(1,1);INSERT INTO mixer_strip(track_id) "
            "VALUES(1),(2),(9);INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) "
            "VALUES(1,1,0,1,'MIDI FX'),(2,1,1,2,'Synth'),(3,1,2,3,'Audio FX');INSERT INTO "
            "state_blobs(hash_blake3,data,size_bytes) VALUES('state',x'01',1);INSERT INTO "
            "plugin_state(device_id,stream_role,state_hash) VALUES(2,'component','state');");
        const auto wave = root.path() / "source.wav";
        std::vector<float> samples(6000);
        for (std::uint64_t i = 0; i < 3000; ++i)
            for (unsigned c = 0; c < 2; ++c)
                samples[static_cast<std::size_t>(i) * 2 + c] = signal(i, c);
        {
            audio::WavWriter writer(wave, 48000, 2, audio::WavFormat::Float32);
            writer.write(samples.data(), 3000);
            writer.close();
        }
        OpRequest request;
        const auto imported = media::prepareImport(*store, wave, 1, 0, request);
        const auto committed = OpJournal(*store).commit(request);
        if (!imported.ok || !committed.ok)
            throw std::runtime_error("source media fixture");
        store->db().exec(
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) "
            "VALUES(1,1,'audio',1,0,62500000),(2,2,'audio',1,0,62500000);INSERT INTO "
            "audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,3000),(2,1,0,3000);");
    }
    render::FreezeCaptureSpec spec(engine::Precision precision = engine::Precision::Float32) const {
        render::FreezeCaptureSpec spec;
        spec.tracks = {1, 2};
        spec.endSample = 1003;
        spec.session = {2, 48000, 64};
        spec.session.precision = precision;
        return spec;
    }
    engine::DeviceLoader loader(Counts &counts, bool poison = false, bool change = false) const {
        return [&counts, poison, change](const engine::DeviceRequest &request, std::string &) {
            ++counts.loads;
            auto result = std::make_unique<Delayed>();
            result->counts = &counts;
            result->index = static_cast<std::size_t>(request.device->id - 1);
            result->delay = std::array<std::int32_t, 3>{13, 7, 16}[result->index];
            result->identity_.version = "runtime-" + std::to_string(request.device->id);
            result->poison = poison && result->index == 2;
            result->change = change && result->index == 2;
            return result;
        };
    }
    std::set<fs::path> stages() const {
        std::set<fs::path> files;
        for (const auto &item : fs::directory_iterator(root.path()))
            if (item.path().filename().string().starts_with(".adi-freeze-"))
                files.insert(item.path());
        return files;
    }
};
} // namespace
