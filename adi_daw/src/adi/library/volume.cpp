// SPDX-License-Identifier: GPL-3.0-or-later
#include "volume.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include <fstream>
#include <random>
#include <stdexcept>
namespace adi::library {
namespace fs = std::filesystem;
namespace {
std::string newId() {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string id(32, '0'); for (auto& c : id) c = hex[random() & 15U];
    return id;
}
bool valid(const std::string& id) {
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
struct Scratch {
    fs::path path;
    ~Scratch() { std::error_code ignored; fs::remove_all(path, ignored); }
};
}
bool MarkerVolumeProvider::identify(const fs::path& root, Volume& out, std::string& error) {
    try {
        const auto canonical = fs::canonical(root);
        if (!fs::is_directory(canonical)) throw std::runtime_error("volume root is not a directory");
        Scratch scratch;
        for (unsigned i = 0; i < 128; ++i) {
            auto candidate = canonical / (".adi-volume-probe-" + newId());
            if (fs::create_directory(candidate)) { scratch.path = std::move(candidate); break; }
        }
        if (scratch.path.empty()) throw std::runtime_error("cannot reserve volume probe");
        { std::ofstream probe(scratch.path / "caseprobe", std::ios::binary); probe << 'x'; probe.close(); if (!probe) throw std::runtime_error("cannot probe case behavior"); }
        const bool insensitive = fs::exists(scratch.path / "CASEPROBE") && fs::equivalent(scratch.path / "caseprobe", scratch.path / "CASEPROBE");
        const auto marker = canonical / ".adi-volume-id";
        if (!fs::exists(marker)) {
            const auto staged = scratch.path / "marker";
            { std::ofstream f(staged, std::ios::binary); f << "ADI-VOLUME-1\n" << newId() << '\n'; f.close(); if (!f) throw std::runtime_error("cannot write volume marker"); }
            const auto result = media::publishFile(staged, marker);
            if (result.status == media::PublishStatus::failed) throw std::runtime_error(result.error);
        }
        if (fs::is_symlink(fs::symlink_status(marker))) throw std::runtime_error("volume marker must not be a symlink");
        std::ifstream f(marker, std::ios::binary); std::string magic, id, extra;
        std::getline(f, magic); std::getline(f, id);
        if (!f || magic != "ADI-VOLUME-1" || !valid(id) || std::getline(f, extra)) throw std::runtime_error("invalid volume marker");
        out = {id, canonical, !insensitive}; error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
}
