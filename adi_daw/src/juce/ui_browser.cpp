// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_browser.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
namespace adi::ui {
namespace {
enum Glyph { None, Favorite, Sounds, Drums, Instruments, AudioFx, MidiFx, Pd, PlugIns, Clips,
             Samples, Grooves, Templates, Packs, User, Project, AddFolder };
void icon(juce::Graphics &g, int kind, juce::Rectangle<float> r) {
    g.setColour(look::text);
    const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
    juce::Path p;
    switch (kind) {
    case Favorite:
        g.setColour(juce::Colour(0xffe5352b));
        g.fillRect(r.reduced(3));
        break;
    case Sounds:
        g.fillEllipse(x + 1, y + h - 5, 4, 4);
        g.fillEllipse(x + w - 6, y + h - 6, 4, 4);
        g.fillRect(x + 4, y + 2, 1.2f, h - 5);
        g.fillRect(x + w - 3, y + 1, 1.2f, h - 5);
        g.fillRect(x + 4, y + 1, w - 6, 1.6f);
        break;
    case Drums:
        for (int i = 0; i < 4; ++i)
            g.drawRect(x + 1 + static_cast<float>(i % 2) * 6, y + 1 + static_cast<float>(i / 2) * 6,
                       4.5f, 4.5f, 1.f);
        break;
    case Instruments:
        g.drawEllipse(r.reduced(1.5f), 1.2f);
        g.fillRect(r.getCentreX() - 0.6f, y + 2, 1.2f, h / 2 - 1);
        break;
    case AudioFx:
        for (int i = 0; i < 6; ++i)
            g.fillRect(x + 1.f + static_cast<float>(i) * 2.2f, y + 2.f + static_cast<float>(i % 3) * 2,
                       1.2f, h - 4 - static_cast<float>(i % 3) * 4);
        break;
    case MidiFx:
        for (int i = 0; i < 3; ++i)
            g.fillRect(x + 1, y + 3.f + static_cast<float>(i) * 4, w - 2 - static_cast<float>(i) * 3, 1.3f);
        break;
    case Pd:
    case PlugIns:
        g.drawRect(x + 4, y + 3, w - 6, h - 6, 1.2f);
        g.fillRect(x, r.getCentreY() - 0.6f, 4.f, 1.2f);
        if (kind == PlugIns) {
            g.fillRect(x + w - 2, y + 5, 2.f, 1.2f);
            g.fillRect(x + w - 2, y + h - 6, 2.f, 1.2f);
        }
        break;
    case Clips:
        g.drawRect(r.reduced(1), 1.2f);
        p.addTriangle(x + 5, y + 4, x + w - 4, r.getCentreY(), x + 5, y + h - 4);
        g.fillPath(p);
        break;
    case Samples:
        g.drawRect(r.reduced(1), 1.2f);
        for (int i = 0; i < 5; ++i)
            g.fillRect(x + 3.f + static_cast<float>(i) * 2.f, y + 4.f + static_cast<float>(i % 2) * 2,
                       1.f, h - 8 - static_cast<float>(i % 2) * 4);
        break;
    case Grooves:
        for (int i = 0; i < 2; ++i) {
            juce::Path wave;
            wave.startNewSubPath(x + 1, y + 5.f + static_cast<float>(i) * 4);
            wave.quadraticTo(x + w / 4, y + 2.f + static_cast<float>(i) * 4, x + w / 2,
                             y + 5.f + static_cast<float>(i) * 4);
            wave.quadraticTo(x + 3 * w / 4, y + 8.f + static_cast<float>(i) * 4, x + w - 1,
                             y + 5.f + static_cast<float>(i) * 4);
            g.strokePath(wave, juce::PathStrokeType(1.2f));
        }
        break;
    case Templates:
        g.drawRect(r.reduced(1), 1.2f);
        g.fillRect(x + 1, y + 5, w - 2, 1.2f);
        g.fillRect(x + w / 2, y + 5, 1.2f, h - 6);
        break;
    case Packs:
        g.drawRect(x + 1, y + 4, w - 5, h - 5, 1.2f);
        g.drawRect(x + 4, y + 1, w - 5, h - 5, 1.2f);
        break;
    case User:
        g.drawEllipse(r.getCentreX() - 3, y + 1, 6, 6, 1.2f);
        p.addCentredArc(r.getCentreX(), y + h, w / 2 - 1, h / 2 - 1, 0,
                        -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
        g.strokePath(p, juce::PathStrokeType(1.2f));
        break;
    case Project:
        g.drawRect(r.reduced(1), 1.2f);
        for (int i = 0; i < 2; ++i)
            g.fillRect(x + 4, y + 5.f + static_cast<float>(i) * 3, w - 7, 1.f);
        break;
    case AddFolder:
        g.drawRect(r.reduced(1), 1.2f);
        g.fillRect(r.getCentreX() - 3, r.getCentreY() - 0.6f, 6.f, 1.2f);
        g.fillRect(r.getCentreX() - 0.6f, r.getCentreY() - 3, 1.2f, 6.f);
        break;
    default:
        break;
    }
}
} // namespace
BrowserPanel::BrowserPanel(AdiRootComponent &root) : root_(root) {
    // Live's sidebar, its names and order (manual 3.3). "Max for Live" is ADI's Pd tier
    // (Master Reference 9.3), Live's place for the same thing.
    sidebar_ = {{"Collections", None, true}, {"Favorites", Favorite},  {{}, None, true},
                {"Categories", None, true},  {"Sounds", Sounds},       {"Drums", Drums},
                {"Instruments", Instruments}, {"Audio Effects", AudioFx}, {"MIDI Effects", MidiFx},
                {"Pd Devices", Pd},          {"Plug-Ins", PlugIns},    {"Clips", Clips},
                {"Samples", Samples},        {"Grooves", Grooves},     {"Templates", Templates},
                {{}, None, true},            {"Places", None, true},   {"Packs", Packs},
                {"User Library", User},      {"Current Project", Project},
                {"Add Folder...", AddFolder}};
    setTitle("Browser");
    setTooltip("Live's browser: your sounds, devices, plug-ins and files.");
    search.setTextToShowWhenEmpty("Search (Cmd + F)", juce::Colour(0xff5a5a5a));
    search.setTitle("Search");
    search.setFont(look::font(12.5f));
    search.setIndents(8, 4);
    search.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    search.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(search);
}
void BrowserPanel::resized() {
    // Live's shape: the search row and the preview row start right of the round
    // buttons; the panes run the panel's width between them, one dark-edged block.
    const auto r = getLocalBounds();
    const int edge = look::border;
    search.setBounds(kStripWidth + edge, edge, std::max(0, r.getWidth() - kStripWidth - 2 * edge), 21);
    const int top = kBarHeight - edge + edge, bottom = r.getHeight() - kBarHeight - edge;
    sidebarArea_ = {edge, top, kSidebarWidth, std::max(0, bottom - top)};
    contentArea_ = {sidebarArea_.getRight() + 2, top,
                    std::max(0, r.getWidth() - sidebarArea_.getRight() - 2 - edge), sidebarArea_.getHeight()};
}
void BrowserPanel::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    const float w = static_cast<float>(getWidth()), h = static_cast<float>(getHeight());
    const float strip = kStripWidth, bar = kBarHeight;
    // The dark edge: the search row, the panes, the preview row, as one block.
    g.setColour(juce::Colour(0xff444444));
    g.fillRoundedRectangle(strip, 0, w - strip, bar + 4, 5.f);
    g.fillRoundedRectangle(0, bar - 4, w, h - 2 * bar + 8, 5.f);
    g.fillRoundedRectangle(strip, h - bar - 4, w - strip, bar + 4, 5.f);
    // The round buttons: show/hide the browser at the top, the groove pool at the foot.
    for (const float y : {0.5f, h - 19.5f}) {
        g.setColour(look::band);
        g.fillEllipse(0, y, 19, 19);
        look::triangle(g, {7, y + 5, 7, 9}, 0, look::panel);
    }
    // The search field.
    g.setColour(look::panel);
    g.fillRect(search.getBounds());
    for (auto area : {sidebarArea_, contentArea_}) {
        g.setColour(look::panel);
        g.fillRect(area);
    }
    // The rows stop at the pane's foot, under the AI tab, as a list does.
    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(sidebarArea_);
        int y = sidebarArea_.getY() + 1;
        for (std::size_t i = 0; i < sidebar_.size(); ++i) {
            const auto &e = sidebar_[i];
            const juce::Rectangle<int> row(sidebarArea_.getX(), y, sidebarArea_.getWidth(), kRowHeight);
            if (static_cast<int>(i) == selected_) {
                g.setColour(look::selection);
                g.fillRect(row);
            }
            if (e.heading) {
                g.setColour(look::textDim);
                g.setFont(look::font(12.f));
                g.drawText(e.name, row.withTrimmedLeft(5), juce::Justification::centredLeft);
            } else {
                icon(g, e.glyph, row.withTrimmedLeft(8).withWidth(13).toFloat().withSizeKeepingCentre(13, 13));
                g.setColour(look::text);
                g.setFont(look::font(12.5f));
                g.drawText(e.name, row.withTrimmedLeft(25), juce::Justification::centredLeft);
            }
            y += kRowHeight;
        }
    }
    // The content pane's header, as Live's list.
    const juce::Rectangle<int> header(contentArea_.getX(), contentArea_.getY(), contentArea_.getWidth(), 19);
    g.setColour(look::columnHead);
    g.fillRect(header);
    g.setColour(look::text);
    g.setFont(look::font(12.f, true));
    g.drawText("Name", header.withTrimmedLeft(4), juce::Justification::centredLeft);
    look::triangle(g, {static_cast<float>(header.getRight()) - 13, static_cast<float>(header.getY()) + 6, 8, 7},
                   juce::MathConstants<float>::halfPi, look::textDim);
    // The preview row: Live's headphones switch at its left.
    const juce::Rectangle<int> preview(kStripWidth + look::border, getHeight() - kBarHeight + 2,
                                       getWidth() - kStripWidth - 2 * look::border, 21);
    g.setColour(look::panel);
    g.fillRect(preview);
    g.setColour(juce::Colour(0xff2e6fd8));
    g.drawEllipse(juce::Rectangle<float>(11, 11).withCentre(
                      {static_cast<float>(preview.getX()) + 13, static_cast<float>(preview.getCentreY())}),
                  2.f);
}
void BrowserPanel::mouseDown(const juce::MouseEvent &e) {
    if (!sidebarArea_.contains(e.getPosition()))
        return;
    const int i = (e.y - sidebarArea_.getY() - 1) / kRowHeight;
    if (i >= 0 && i < static_cast<int>(sidebar_.size()) && !sidebar_[static_cast<std::size_t>(i)].heading) {
        selected_ = i;
        repaint();
        // The content pane is the browser's next piece (LANES §3b item 2).
        root_.say((sidebar_[static_cast<std::size_t>(i)].name + " is not listed yet.").toStdString());
    }
}
} // namespace adi::ui
