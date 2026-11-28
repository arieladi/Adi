// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/ui/project_media.hpp"
#include "ui_look.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace adi::ui {
class ProjectMediaPanel final : public juce::Component, private juce::TableListBoxModel {
public:
    explicit ProjectMediaPanel(bool writable) : writable_(writable) {
        setTitle("Project Files");
        table_.setModel(this);
        table_.setTitle("Project media files");
        table_.setRowHeight(22);
        auto &header = table_.getHeader();
        // The table keeps the media inventory's row order.
        constexpr auto columnFlags = juce::TableHeaderComponent::notSortable;
        header.addColumn("File", Name, 180, 80, -1, columnFlags);
        header.addColumn("Location", Location, 360, 100, -1, columnFlags);
        header.addColumn("Status", State, 90, 64, -1, columnFlags);
        header.addColumn("Size", Size, 110, 68, -1, columnFlags);
        table_.setAutoSizeMenuOptionShown(false);
        header.setPopupMenuActive(false);
        addAndMakeVisible(table_);
        status_.setText("Checking project files...", juce::dontSendNotification);
        addAndMakeVisible(status_);
        locate_.setEnabled(false);
        locate_.onClick = [this] { (void)locateSelected(); };
        addAndMakeVisible(locate_);
        setSize(780, 400);
    }
    ~ProjectMediaPanel() override { table_.setModel(nullptr); }

    void resized() override {
        auto area = getLocalBounds().reduced(8);
        auto buttons = area.removeFromBottom(26);
        locate_.setBounds(buttons.removeFromRight(125));
        status_.setBounds(buttons);
        table_.setBounds(area.withTrimmedBottom(6));
    }
    void paint(juce::Graphics &g) override { g.fillAll(look::panel); }

    void scan(ProjectMediaInventory inventory) {
        worker_.request_stop();
        const auto owner = juce::Component::SafePointer<ProjectMediaPanel>(this);
        status_.setText("Checking project files...", juce::dontSendNotification);
        locate_.setEnabled(false);
        scanning_ = true;
        worker_ = std::jthread([owner, inventory = std::move(inventory)](std::stop_token stop) mutable {
            try {
                inspectProjectMedia(inventory, stop);
                if (stop.stop_requested()) return;
                juce::MessageManager::callAsync([owner, stop, files = std::move(inventory.files)]() mutable {
                    if (owner && !stop.stop_requested()) owner->setFiles(std::move(files));
                });
            } catch (const std::exception &exception) {
                const auto error = juce::String::fromUTF8(exception.what());
                juce::MessageManager::callAsync([owner, stop, error] {
                    if (owner && !stop.stop_requested()) {
                        owner->scanning_ = false;
                        owner->status_.setText(error, juce::dontSendNotification);
                    }
                });
            }
        });
    }
    void setFiles(std::vector<ProjectMediaFile> files) {
        scanning_ = false;
        files_ = std::move(files);
        table_.updateContent();
        table_.deselectAllRows();
        locate_.setEnabled(false);
        status_.setText(files_.empty() ? "This project has no media files." : juce::String{}, juce::dontSendNotification);
        table_.repaint();
    }
    const std::vector<ProjectMediaFile> &files() const noexcept { return files_; }
    void select(int row) { table_.selectRow(row); }
    void showStatus(const std::string &text) { status_.setText(juce::String::fromUTF8(text.c_str()), juce::dontSendNotification); }
    bool locateSelected() {
        const auto row = table_.getSelectedRow();
        if (scanning_ || !writable_ || !locateFile || row < 0 || row >= getNumRows()) return false;
        return locateFile(files_[static_cast<std::size_t>(row)]);
    }
    std::function<bool(const ProjectMediaFile &)> locateFile;

private:
    enum Column { Name = 1, Location, State, Size };
    int getNumRows() override { return static_cast<int>(files_.size()); }
    void selectedRowsChanged(int row) override {
        locate_.setEnabled(!scanning_ && writable_ && row >= 0 && row < getNumRows());
    }
    void cellDoubleClicked(int row, int, const juce::MouseEvent &) override {
        select(row);
        (void)locateSelected();
    }
    void returnKeyPressed(int) override { (void)locateSelected(); }
    void paintRowBackground(juce::Graphics &g, int row, int, int, bool selected) override {
        g.fillAll(selected ? look::selection : row % 2 != 0 ? look::band : look::panel);
    }
    juce::String getCellTooltip(int row, int column) override {
        if (row < 0 || row >= getNumRows()) return {};
        const auto &file = files_[static_cast<std::size_t>(row)];
        if (column == Location) return juce::String::fromUTF8(media::pathUtf8(file.location).c_str());
        if (column == State) return juce::String::fromUTF8(file.problem.c_str());
        return {};
    }
    void paintCell(juce::Graphics &g, int row, int column, int width, int height, bool) override {
        if (row < 0 || row >= getNumRows()) return;
        const auto &file = files_[static_cast<std::size_t>(row)];
        juce::String text;
        switch (column) {
        case Name: text = juce::String::fromUTF8(file.name.c_str()); break;
        case Location: text = juce::String::fromUTF8(media::pathUtf8(file.location).c_str()); break;
        case State: text = file.problem.empty() ? "Available" : "Missing"; break;
        case Size: if (file.bytes) text = juce::File::descriptionOfSizeInBytes(*file.bytes); break;
        default: break;
        }
        g.setColour(column == State && !file.problem.empty() ? look::error : look::text);
        g.setFont(look::font(look::fontHeading));
        g.drawText(text, juce::Rectangle<int>{6, 0, std::max(0, width - 12), height}, juce::Justification::centredLeft, true);
    }

    const bool writable_;
    bool scanning_ = false;
    juce::TableListBox table_;
    juce::Label status_;
    juce::TextButton locate_{"Locate File..."};
    std::vector<ProjectMediaFile> files_;
    std::jthread worker_;
};
} // namespace adi::ui
