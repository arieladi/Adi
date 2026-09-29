// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/transport.hpp"
#include "adi/ui/panel_layout.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <string>
namespace adi {
class Store;
}
namespace adi::ui {
struct DesktopDefaults {
    int browser = 240, mixer = 280, deviceHeight = 200;
};
struct WindowState {
    explicit WindowState(DesktopDefaults d = {})
        : panels({Panel::Browser, d.browser, 120, false}, {Panel::Mixer, d.mixer, 160, false}, 240),
          deviceHeight(d.deviceHeight) {}
    PanelLayout panels;
    double zoom = 1;
    double timelineLeft = 0, pixelsPerQuarter = 48;
    int laneHeight = 64;
    std::map<std::int64_t, int> laneHeights;
    std::set<std::int64_t> foldedDevices;
    int deviceHeight;
    bool docked = true;
};
class ViewStateStore {
  public:
    explicit ViewStateStore(Store &s) : store_(s) {}
    WindowState load(const std::string &window, DesktopDefaults defaults = {}) const;
    bool save(const std::string &window, const WindowState &, std::string &error);

  private:
    Store &store_;
};
class DirtySet {
  public:
    enum Region : unsigned { Transport = 1, Layout = 2, Content = 4, All = 7 };
    void mark(unsigned bits) noexcept { bits_ |= bits; }
    unsigned drain() noexcept {
        const auto b = bits_;
        bits_ = 0;
        return b;
    }

  private:
    unsigned bits_ = All;
};
// One UI producer and one driver consumer. No UI ever reads/writes Transport.
class TransportMailbox {
  public:
    enum class Command { Play, Stop, Locate };
    bool hasRoom(std::size_t count = 1) const noexcept {
        return count <= queue_.size() &&
               write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire) <=
                   queue_.size() - count;
    }
    bool post(Command c, std::int64_t sample = 0) noexcept {
        auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= queue_.size())
            return false;
        queue_[w % queue_.size()] = {c, sample};
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    void drain(engine::Transport &transport) noexcept {
        auto r = read_.load(std::memory_order_relaxed);
        const auto w = write_.load(std::memory_order_acquire);
        while (r != w) {
            const auto item = queue_[r % queue_.size()];
            if (item.command == Command::Locate)
                transport.locate(item.sample);
            else
                transport.play(item.command == Command::Play);
            ++r;
        }
        publish(transport);
        read_.store(r, std::memory_order_release);
    }
    void publish(const engine::Transport &t) noexcept {
        playing_.store(t.playing(), std::memory_order_relaxed);
        position_.store(t.position(), std::memory_order_relaxed);
    }
    // UI producer sees the last queued intent until the driver acknowledges it.
    bool desiredPlaying() const noexcept {
        const auto w = write_.load(std::memory_order_relaxed);
        const auto r = read_.load(std::memory_order_acquire);
        for (auto i = w; i != r;) {
            const auto c = queue_[--i % queue_.size()].command;
            if (c != Command::Locate)
                return c == Command::Play;
        }
        return playing();
    }
    void setSampleRate(double rate) noexcept { sampleRate_.store(rate, std::memory_order_relaxed); }
    double sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    bool playing() const noexcept { return playing_.load(std::memory_order_relaxed); }
    std::int64_t position() const noexcept { return position_.load(std::memory_order_relaxed); }

  private:
    struct Item {
        Command command = Command::Stop;
        std::int64_t sample = 0;
    };
    std::array<Item, 16> queue_{};
    std::atomic<std::size_t> write_{0}, read_{0};
    std::atomic<bool> playing_{false};
    std::atomic<double> sampleRate_{0};
    std::atomic<std::int64_t> position_{0};
};
} // namespace adi::ui
