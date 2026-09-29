// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/transport.hpp"
#include "adi/ui/panel_layout.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
namespace adi {
class Store;
}
namespace adi::ui {
struct DesktopDefaults {
    int browser = 240, mixer = 280;
    // The device strip's fixed minimum: Live's Device View at 100 % interface
    // zoom, 169 zoom-independent units, the director's figure and UNVERIFIED
    // (ADR-0203 d7, measured under d26). A desktop default handed in by the
    // shell, never a constant inside a component (ADR-0200 d5, ADR-0180 d9).
    int deviceFloor = 169;
};
struct WindowState {
    explicit WindowState(DesktopDefaults d = {})
        : panels({Panel::Browser, d.browser, 120, false}, {Panel::Mixer, d.mixer, 160, false}, 240),
          deviceHeight(d.deviceFloor), deviceFloor(d.deviceFloor) {}
    PanelLayout panels;
    double zoom = 1; // the arrangement's own zoom; interface zoom is the window's
    int deviceHeight;
    int deviceFloor; // from DesktopDefaults, never persisted
    bool docked = true;
    bool aiChatOpen = false; // the AI tab at the browser's foot, collapsed as Live's column
    // The strip expands upward only: a height below the floor becomes the floor.
    void setDeviceHeight(int h) noexcept { deviceHeight = h < deviceFloor ? deviceFloor : h; }
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
    enum Region : unsigned { Transport = 1, Layout = 2, Content = 4, Status = 8, All = 15 };
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
    enum class Command { Play, Stop };
    bool hasRoom() const noexcept {
        return write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire) <
               queue_.size();
    }
    bool post(Command c) noexcept {
        auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= queue_.size())
            return false;
        queue_[w % queue_.size()] = c;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    void drain(engine::Transport &transport) noexcept {
        auto r = read_.load(std::memory_order_relaxed);
        const auto w = write_.load(std::memory_order_acquire);
        while (r != w) {
            transport.play(queue_[r % queue_.size()] == Command::Play);
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
        return w != read_.load(std::memory_order_acquire)
                   ? queue_[(w - 1) % queue_.size()] == Command::Play
                   : playing();
    }
    void setSampleRate(double rate) noexcept { sampleRate_.store(rate, std::memory_order_relaxed); }
    double sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    bool playing() const noexcept { return playing_.load(std::memory_order_relaxed); }
    std::int64_t position() const noexcept { return position_.load(std::memory_order_relaxed); }

  private:
    std::array<Command, 16> queue_{};
    std::atomic<std::size_t> write_{0}, read_{0};
    std::atomic<bool> playing_{false};
    std::atomic<double> sampleRate_{0};
    std::atomic<std::int64_t> position_{0};
};
} // namespace adi::ui
