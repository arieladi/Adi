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
    // Live 11's widths at 100 %: the browser with the frame's margin, and the track
    // header's In/Out and mixer columns with the meter and the right-hand strip,
    // 78 + 116 + 27 (measured 2026-10-27; ui_mixer.hpp's MixerPanel::kFullWidth).
    int browser = 316, mixer = 221;
    // The device strip's fixed minimum: Live's Device View at 100 % interface zoom
    // (ADR-0203 d7). Measured on Live 11.2.7, the director's Mac, 2026-10-27: the
    // whole Device View panel is 196 points with its 4 pt edges, its device body
    // under the title bars about 170, which is the director's 169. ADI's strip is the
    // whole panel, so its floor is 196, to be read again on Live 12 (ADR-0203 d26). A desktop default
    // handed in by the shell, never a constant in a component (ADR-0200 d5).
    int deviceFloor = 196;
};
struct WindowState {
    explicit WindowState(DesktopDefaults d = {})
        : panels({Panel::Browser, d.browser, 120, false}, {Panel::Mixer, d.mixer, 120, false}, 240),
          deviceHeight(d.deviceFloor), deviceFloor(d.deviceFloor) {}
    PanelLayout panels;
    double zoom = 1; // the arrangement's own zoom; interface zoom is the window's
    int deviceHeight;
    int deviceFloor; // from DesktopDefaults, never persisted
    bool docked = true;
    bool aiChatOpen = false; // the AI tab at the browser's foot, collapsed as Live's column
    // The arrangement's own view: where it starts, its time zoom and its lane heights.
    double timelineLeft = 0, pixelsPerQuarter = 48;
    int laneHeight = 88; // Live 11's default track: five rows of its controls
    std::map<std::int64_t, int> laneHeights;
    std::set<std::int64_t> foldedDevices; // devices folded in the strip, per window
    bool mixerView = false;               // Live's Mixer View, hidden until asked for
    int mixerViewHeight = 220;
    // Live's Follow (manual §6.2): the arrangement keeps the playhead on screen while it plays.
    bool follow = false;
    // The strip expands upward only: a height below the floor becomes the floor.
    void setDeviceHeight(int h) noexcept { deviceHeight = h < deviceFloor ? deviceFloor : h; }
};
struct FloatingState {
    std::string monitor;
    int x=100,y=100,width=800,height=420;
    bool open=false;
};
class ViewStateStore {
  public:
    explicit ViewStateStore(Store &s) : store_(s) {}
    FloatingState loadFloating(const std::string& key) const;
    bool saveFloating(const std::string& key,const FloatingState&,std::string& error);
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
