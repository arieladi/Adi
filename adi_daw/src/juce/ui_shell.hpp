// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/project_view.hpp"
#include "adi/settings/store.hpp"
#include "adi/ui/op_submitter.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include "adi/ui/window_state.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <optional>
namespace adi::ui {
class AdiRootComponent;
// The app-scoped command layer (ADR-0180 d6): one manager, installed on every window.
// The keys are Live 12's (manual §41, ADR-0203 d6) unless a preset says otherwise.
class AppCommands final : public juce::ApplicationCommandTarget {
  public:
    enum Id {
        PlayStop = 0x7100,
        Stop,
        Undo,
        Redo,
        SwapPanels,
        ToggleBrowser,
        ToggleMixer,
        ToggleDeviceView,
        ToggleInfoView,
        ZoomIn,
        ZoomOut,
        LastId = ZoomOut
    };
    explicit AppCommands(settings::AppSettings &);
    void reload();
    bool key(const juce::KeyPress &, AdiRootComponent &);
    void detach(AdiRootComponent &);
    juce::ApplicationCommandTarget *getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID> &) override;
    void getCommandInfo(juce::CommandID, juce::ApplicationCommandInfo &) override;
    bool perform(const InvocationInfo &) override;
    // App-scope view settings every window reads (ADR-0131, ADR-0203 d12).
    bool infoView() const noexcept { return infoView_; }
    bool setInfoView(bool on, std::string &why);
    bool tooltips() const noexcept { return tooltips_; }
    int tooltipDelayMs() const noexcept { return tooltipDelayMs_; }
    juce::ApplicationCommandManager manager;

  private:
    settings::AppSettings &settings_;
    AdiRootComponent *active_ = nullptr;
    bool live_ = true;
    bool infoView_ = true;
    bool tooltips_ = true;
    int tooltipDelayMs_ = 600;
};
class TransportBar final : public juce::Component {
  public:
    explicit TransportBar(AdiRootComponent &);
    void paint(juce::Graphics &) override;
    void resized() override;
    juce::TextButton play{"Play / stop"}, stop{"Stop"};

  private:
    AdiRootComponent &root_;
};
// Live's Status Bar (manual 3.2): messages along the window's foot, with the
// Info View's show/hide toggle in its bottom-left corner (manual 2.2.2).
class StatusBar final : public juce::Component {
  public:
    explicit StatusBar(AdiRootComponent &);
    void paint(juce::Graphics &) override;
    void resized() override;
    juce::TextButton infoToggle{"?"};

  private:
    AdiRootComponent &root_;
};
class AdiRootComponent final : public juce::Component {
  public:
    AdiRootComponent(engine::ProjectView &, OpSubmitter &, TransportMailbox &, AppCommands &,
                     ViewStateStore &, std::string window, DesktopDefaults defaults = {});
    ~AdiRootComponent() override;
    void frame() noexcept;
    void paint(juce::Graphics &) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress &) override;
    bool command(int);
    void mark(unsigned bits) noexcept { dirty_.mark(bits); }
    const SnapshotReader &reader() const noexcept { return *reader_; }
    bool playing() const noexcept { return playing_; }
    double bpm() const noexcept;
    std::uint64_t generation() const noexcept { return generation_; }
    unsigned lastDrain() const noexcept { return lastDrain_; }
    const std::string &error() const noexcept { return error_; }
    WindowState &state() noexcept { return state_; }
    bool persist();
    // The window's own interface zoom (ADR-0203 d6, A5-01): each window keeps its
    // own, and the ZoomHost around the root applies it as a transform.
    float interfaceZoom() const noexcept { return interfaceZoom_; }
    // What the Info View shows: the element under the mouse, read in the frame.
    const juce::String &infoText() const noexcept { return infoText_; }
    void hover(juce::Component *);
    // Laid-out rectangles, in the root's logical units, for tests and children.
    juce::Rectangle<int> deviceArea() const noexcept { return deviceArea_; }
    juce::Rectangle<int> infoArea() const noexcept { return infoArea_; }
    juce::Rectangle<int> browserArea() const noexcept { return browserArea_; }
    juce::Rectangle<int> chatArea() const noexcept { return chatArea_; }
    TransportBar transport;
    StatusBar status;
    juce::TextButton aiTab{"AI"};

  private:
    bool fail(std::string message);
    bool toggle(Panel);
    engine::ProjectView &view_;
    OpSubmitter &submitter_;
    TransportMailbox &mailbox_;
    AppCommands &commands_;
    ViewStateStore &persistence_;
    std::string window_;
    WindowState state_;
    DirtySet dirty_;
    std::optional<SnapshotReader> reader_;
    std::uint64_t generation_ = 0;
    unsigned lastDrain_ = 0;
    bool playing_ = false;
    bool infoShown_ = true;
    std::int64_t position_ = 0;
    float interfaceZoom_ = 1;
    juce::Component *hovered_ = nullptr;
    juce::String infoText_;
    juce::Rectangle<int> deviceArea_, infoArea_, browserArea_, chatArea_;
    int leftWidth_ = 0, rightWidth_ = 0;
    std::string error_;
};
// Holds a root at its window's interface zoom: the root lays out in logical
// units and the transform scales it, so nothing below knows the zoom exists.
class ZoomHost final : public juce::Component {
  public:
    explicit ZoomHost(std::unique_ptr<AdiRootComponent>);
    void resized() override;
    AdiRootComponent &root() noexcept { return *root_; }

  private:
    std::unique_ptr<AdiRootComponent> root_;
};
// One attachment, key binding and tooltip window per native window, sharing
// the app command manager.
class AdiWindow final : public juce::DocumentWindow, private juce::KeyListener {
  public:
    // `onDesktop` false builds the window without a native peer, for tests.
    AdiWindow(std::unique_ptr<AdiRootComponent>, AppCommands &, bool onDesktop = true);
    ~AdiWindow() override;
    using juce::DocumentWindow::keyPressed;
    void closeButtonPressed() override { setVisible(false); }
    AdiRootComponent &root() noexcept { return *root_; }
    const juce::TooltipWindow *tooltips() const noexcept { return tips_.get(); }
    int tooltipDelayMs() const noexcept { return tipDelayMs_; }

  private:
    bool keyPressed(const juce::KeyPress &k, juce::Component *) override;
    AdiRootComponent *root_;
    AppCommands &commands_;
    std::unique_ptr<juce::TooltipWindow> tips_;
    int tipDelayMs_ = 0;
    juce::VBlankAttachment clock_;
};
} // namespace adi::ui
