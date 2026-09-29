// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/project_view.hpp"
#include "adi/settings/store.hpp"
#include "adi/ui/op_submitter.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include "adi/ui/window_state.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
namespace adi::ui {
class AdiRootComponent;
class AppCommands final : public juce::ApplicationCommandTarget {
  public:
    enum Id {
        PlayStop = 0x7100,
        Stop,
        Undo,
        Redo,
        SwapPanels,
        NewProject,
        OpenProject,
        SaveProject,
        AudioSettings
    };
    explicit AppCommands(const settings::AppSettings &);
    void reload(const settings::AppSettings &);
    bool key(const juce::KeyPress &, AdiRootComponent &);
    void detach(AdiRootComponent &);
    void activate(AdiRootComponent &root);
    juce::ApplicationCommandTarget *getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID> &) override;
    void getCommandInfo(juce::CommandID, juce::ApplicationCommandInfo &) override;
    bool perform(const InvocationInfo &) override;
    juce::ApplicationCommandManager manager;

  private:
    AdiRootComponent *active_ = nullptr;
    bool live_ = true;
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
    bool canUndo(bool redo) const { return redo ? submitter_.canRedo() : submitter_.canUndo(); }
    std::string undoTitle(bool redo) const {
        auto step = redo ? submitter_.nextRedo() : submitter_.nextUndo();
        return std::string(redo ? "Redo" : "Undo") +
               (step && !step->label.empty() ? " " + step->label : "");
    }
    std::function<bool(int)> applicationCommand;
    std::function<void()> afterEdit;
    TransportBar transport;

  private:
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
    std::int64_t position_ = 0;
    double sampleRate_ = 0;
    std::string error_;
};
class ShellMenu;
// One attachment and key binding per native window, sharing the app command manager.
class AdiWindow final : public juce::DocumentWindow, private juce::KeyListener {
  public:
    AdiWindow(std::unique_ptr<AdiRootComponent>, AppCommands &);
    ~AdiWindow() override;
    using juce::DocumentWindow::keyPressed;
    std::function<void()> onClose;
    void closeButtonPressed() override {
        if (onClose)
            onClose();
        else
            setVisible(false);
    }

  private:
    bool keyPressed(const juce::KeyPress &k, juce::Component *) override;
    AdiRootComponent *root_;
    AppCommands &commands_;
    juce::VBlankAttachment clock_;
    std::unique_ptr<ShellMenu> menu_;
};
} // namespace adi::ui
