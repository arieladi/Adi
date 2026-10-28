// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_settings.hpp"
#include "ui_look.hpp"
#include <cmath>
namespace adi::ui {
namespace {
using settings::Scope;
using settings::Type;
using settings::Value;
// A value as the page shows it: words as they are, nothing as Live's "None", numbers
// plainly, a list one per ";".
juce::String text(const Value &v) {
    if (v.is_string())
        return v.get<std::string>().empty() ? juce::String("None") : juce::String::fromUTF8(v.get<std::string>().c_str());
    if (v.is_boolean())
        return v.get<bool>() ? "On" : "Off";
    if (v.is_number_integer())
        return juce::String(v.get<std::int64_t>());
    if (v.is_number())
        return juce::String(v.get<double>());
    if (v.is_array()) {
        juce::StringArray parts;
        for (const auto &item : v)
            parts.add(text(item));
        return parts.joinIntoString("; ");
    }
    return {};
}
// A list typed in the box: one entry per ";", blanks dropped.
Value list(const juce::String &typed) {
    Value out = Value::array();
    for (auto part : juce::StringArray::fromTokens(typed, ";", ""))
        if (part.trim().isNotEmpty())
            out.push_back(part.trim().toStdString());
    return out;
}
// Live's switch: a box that reads On, lit, or Off. With the focus, either arrow or Enter
// switches it, as a click does (manual 40.1.2).
class SettingSwitch final : public juce::TextButton {
  public:
    bool keyPressed(const juce::KeyPress &k) override {
        const bool key = k == juce::KeyPress::upKey || k == juce::KeyPress::downKey ||
                         k == juce::KeyPress::returnKey;
        if (!key || !isEnabled())
            return juce::TextButton::keyPressed(k);
        setToggleState(!getToggleState(), juce::dontSendNotification);
        if (onClick)
            onClick();
        return true;
    }
};
} // namespace
SettingRow::SettingRow(SettingsPanel &panel, const settings::Setting &s)
    : setting(s), panel_(panel) {
    setTitle(juce::String::fromUTF8(s.label.c_str()));
    setTooltip(juce::String::fromUTF8(s.help.c_str()));
    switch (s.type) {
    case Type::Bool: {
        auto button = std::make_unique<SettingSwitch>();
        button->setClickingTogglesState(true);
        button->setColour(juce::TextButton::buttonColourId, look::button);
        auto *raw = button.get();
        button->onClick = [this, raw] { commit(raw->getToggleState()); };
        control = std::move(button);
        break;
    }
    case Type::Choice: {
        auto chooser = std::make_unique<juce::ComboBox>();
        int id = 1;
        for (const auto &choice : s.choices)
            chooser->addItem(text(choice), id++);
        auto *raw = chooser.get();
        chooser->onChange = [this, raw] {
            const int i = raw->getSelectedItemIndex();
            if (i >= 0 && static_cast<std::size_t>(i) < setting.choices.size())
                commit(setting.choices[static_cast<std::size_t>(i)]);
        };
        control = std::move(chooser);
        break;
    }
    case Type::Int:
    case Type::Real:
        if (s.max > s.min) {
            // Live's value box: drag it, or type into it.
            auto box = std::make_unique<juce::Slider>(juce::Slider::LinearBar, juce::Slider::TextBoxLeft);
            box->setRange(s.min, s.max, s.type == Type::Int ? 1. : 0.);
            box->setTextBoxIsEditable(true);
            box->setScrollWheelEnabled(false);
            // With the focus, the arrows step it: by one, or a hundredth of its range.
            box->setWantsKeyboardFocus(true);
            box->setNumDecimalPlacesToDisplay(s.type == Type::Int ? 0 : 2);
            box->setColour(juce::Slider::textBoxTextColourId, look::text);
            auto *raw = box.get();
            auto send = [this, raw] {
                if (setting.type == Type::Int)
                    commit(static_cast<std::int64_t>(std::llround(raw->getValue())));
                else
                    commit(raw->getValue());
            };
            box->onDragEnd = send;
            box->onValueChange = [raw, send] {
                if (!raw->isMouseButtonDown())
                    send();
            };
            control = std::move(box);
            break;
        }
        [[fallthrough]];
    case Type::Text:
    case Type::Path:
    case Type::PathList: {
        auto editor = std::make_unique<juce::TextEditor>();
        editor->setFont(look::font(12.f));
        editor->setIndents(4, 2);
        auto *raw = editor.get();
        auto send = [this, raw] {
            switch (setting.type) {
            case Type::Int:
                commit(static_cast<std::int64_t>(raw->getText().getLargeIntValue()));
                break;
            case Type::Real:
                commit(raw->getText().getDoubleValue());
                break;
            case Type::PathList:
                commit(list(raw->getText()));
                break;
            default:
                commit(raw->getText().toStdString());
            }
        };
        editor->onReturnKey = send;
        editor->onFocusLost = send;
        control = std::move(editor);
        break;
    }
    }
    // A project's settings are changed by their ops and a device's are kept with the
    // device: shown here with their mark, changed where they live (R-03).
    control->setEnabled(s.scope == Scope::App);
    control->setTitle(getTitle());
    addAndMakeVisible(*control);
    addMouseListener(this, true);
    refresh();
}
void SettingRow::refresh() {
    const auto value = panel_.store.get(setting.key);
    if (auto *button = dynamic_cast<juce::TextButton *>(control.get())) {
        const bool on = value.is_boolean() && value.get<bool>();
        button->setToggleState(on, juce::dontSendNotification);
        button->setButtonText(on ? "On" : "Off");
    } else if (auto *chooser = dynamic_cast<juce::ComboBox *>(control.get())) {
        int index = -1;
        for (std::size_t i = 0; i < setting.choices.size(); ++i)
            if (setting.choices[i] == value)
                index = static_cast<int>(i);
        chooser->setSelectedItemIndex(index, juce::dontSendNotification);
    } else if (auto *box = dynamic_cast<juce::Slider *>(control.get())) {
        if (value.is_number())
            box->setValue(value.get<double>(), juce::dontSendNotification);
    } else if (auto *editor = dynamic_cast<juce::TextEditor *>(control.get())) {
        // A typed field shows what is stored, an empty one empty.
        if (!editor->hasKeyboardFocus(true))
            editor->setText(value.is_string() ? juce::String::fromUTF8(value.get<std::string>().c_str()) : text(value),
                            false);
    }
}
void SettingRow::commit(const Value &v) {
    panel_.change(setting, v);
    refresh();
}
void SettingRow::resized() {
    // Live's controls sit at one place on every page, sized by kind.
    int width = kWide;
    switch (setting.type) {
    case Type::Bool:
    case Type::Int:
    case Type::Real:
        width = kNarrow;
        break;
    case Type::Text:
    case Type::Path:
    case Type::PathList:
        width = std::max(kWide, getWidth() - kControlX - 16);
        break;
    case Type::Choice:
        break;
    }
    width = std::min(width, std::max(40, getWidth() - kControlX - 8));
    control->setBounds(kControlX, 3, width, kHeight - 6);
}
void SettingRow::focusOfChildComponentChanged(FocusChangeType cause) {
    // Marked where the keyboard put the focus, as Live 12's focus ring; a click marks nothing.
    const bool now = control->hasKeyboardFocus(true) && cause != focusChangedByMouseClick;
    if (now != keyFocus_) {
        keyFocus_ = now;
        repaint();
    }
}
void SettingRow::paint(juce::Graphics &g) {
    if (keyFocus_) {
        g.setColour(look::accent);
        g.drawRect(control->getBounds().expanded(1), 1);
    }
    g.setColour(setting.scope == Scope::App ? look::text : look::textDim);
    g.setFont(look::font(12.5f));
    int right = kControlX - 8;
    // The scope's mark beside the name when the value is not the application's (R-03).
    if (setting.scope != Scope::App) {
        const juce::Rectangle<float> pill(static_cast<float>(right - 48), 4, 46, kHeight - 8);
        right -= 52;
        g.setColour(look::field);
        g.fillRoundedRectangle(pill, pill.getHeight() / 2);
        g.setColour(look::text);
        g.setFont(look::font(10.5f));
        g.drawText(setting.scope == Scope::Project ? "Project" : "Device", pill, juce::Justification::centred);
        g.setColour(look::textDim);
        g.setFont(look::font(12.5f));
    }
    g.drawText(getTitle(), kLabelX, 0, std::max(0, right - kLabelX), kHeight, juce::Justification::centredLeft,
               true);
}
void SettingRow::mouseEnter(const juce::MouseEvent &) {
    panel_.setHelp(getTitle() + ": " + getTooltip());
}
SettingsHeading::SettingsHeading(const juce::String &words) {
    setTitle(words);
    setInterceptsMouseClicks(false, false);
}
void SettingsHeading::paint(juce::Graphics &g) {
    const auto font = look::font(12.5f, true);
    g.setFont(font);
    g.setColour(look::text);
    const int x = SettingRow::kLabelX;
    g.drawText(getTitle(), x, 0, getWidth() - x, getHeight(), juce::Justification::centredLeft);
    const int end = x + juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, getTitle())) + 12;
    g.setColour(look::outline);
    g.fillRect(end, getHeight() / 2, std::max(0, getWidth() - end - 16), 1);
}
SettingsPageList::SettingsPageList(SettingsPanel &panel) : panel_(panel) {
    setWantsKeyboardFocus(true);
    setTitle("Settings pages");
}
juce::StringArray SettingsPageList::lines(const std::string &page) {
    juce::StringArray out;
    for (auto word : juce::StringArray::fromTokens(juce::String::fromUTF8(page.c_str()), " ", ""))
        if (word != "&" && word.isNotEmpty())
            out.add(word.trimCharactersAtEnd(","));
    return out;
}
std::vector<juce::Rectangle<int>> SettingsPageList::tabs() const {
    // Live's tabs: 19 units for a line and 15 for each more, 7 between them.
    std::vector<juce::Rectangle<int>> out;
    int y = 6;
    for (const auto &name : panel_.pageNames()) {
        const int height = 19 + 15 * std::max(0, lines(name).size() - 1);
        out.emplace_back(7, y, getWidth() - 7, height);
        y += height + 7;
    }
    return out;
}
void SettingsPageList::paint(juce::Graphics &g) {
    const auto rects = tabs();
    const auto &names = panel_.pageNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        const bool open = !panel_.finding() && names[i] == panel_.page();
        // The open tab runs on into its page; the others stop short of it.
        const auto r = open ? rects[i].toFloat() : rects[i].toFloat().withTrimmedRight(7);
        g.setColour(look::panel);
        if (open)
            g.fillRect(r.withTrimmedLeft(8));
        g.fillRoundedRectangle(r, 4.f);
        g.setColour(look::text);
        g.setFont(look::font(12.5f, true));
        int y = rects[i].getY() + 2;
        for (const auto &line : lines(names[i])) {
            g.drawText(line, rects[i].getX() + 10, y, rects[i].getWidth() - 12, 15, juce::Justification::centredLeft);
            y += 15;
        }
    }
}
void SettingsPageList::mouseDown(const juce::MouseEvent &e) {
    const auto rects = tabs();
    for (std::size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(e.getPosition()))
            panel_.showPage(panel_.pageNames()[i]);
}
bool SettingsPageList::keyPressed(const juce::KeyPress &k) {
    // With the chooser focused, the arrows move between the pages (Live 12 §40.1.2).
    const auto &names = panel_.pageNames();
    const auto at = std::find(names.begin(), names.end(), panel_.page());
    if (at == names.end())
        return false;
    if (k.getKeyCode() == juce::KeyPress::downKey && at + 1 != names.end()) {
        panel_.showPage(*(at + 1));
        return true;
    }
    if (k.getKeyCode() == juce::KeyPress::upKey && at != names.begin()) {
        panel_.showPage(*(at - 1));
        return true;
    }
    return false;
}
SettingsPanel::SettingsPanel(settings::AppSettings &settingsStore,
                             std::function<std::unique_ptr<juce::Component>()> audioDevices)
    : store(settingsStore), pageNames_(settings::pages()), makeAudio_(std::move(audioDevices)) {
    setLookAndFeel(&look::liveLook());
    setTitle("Settings");
    setWantsKeyboardFocus(true);
    search.setTextToShowWhenEmpty("Find a setting", juce::Colour(0xff5a5a5a));
    search.setTitle("Find a setting");
    search.setFont(look::font(12.5f));
    search.setIndents(8, 3);
    search.onTextChange = [this] { find(search.getText()); };
    search.onEscapeKey = [this] {
        if (onClose)
            onClose();
    };
    defaults.setTooltip("Puts every setting on this page back to its default.");
    defaults.setColour(juce::TextButton::buttonColourId, look::button);
    defaults.onClick = [this] {
        store.resetPage(page_, {settings::Actor::User, {}, juce::Time::currentTimeMillis() * 1000});
        if (onChange)
            onChange();
        std::string why;
        if (!store.save(why))
            setHelp("Not saved: " + juce::String::fromUTF8(why.c_str()));
        for (auto &row : rows_)
            row->refresh();
    };
    scroll_.setViewedComponent(&content_, false);
    scroll_.setScrollBarsShown(true, false);
    for (auto *c : std::initializer_list<juce::Component *>{&search, &pageList, &scroll_, &defaults})
        addAndMakeVisible(c);
    if (!pageNames_.empty())
        showPage(pageNames_.front());
}
SettingsPanel::~SettingsPanel() { setLookAndFeel(nullptr); }
void SettingsPanel::showPage(const std::string &name) {
    if (search.getText().isNotEmpty())
        search.clear();
    page_ = name;
    rows_.clear();
    headings_.clear();
    order_.clear();
    // The Audio page opens with Live's device choosers: driver, devices, rate, buffer.
    if (name == "Audio" && makeAudio_ && !audio_) {
        audio_ = makeAudio_();
        if (audio_)
            content_.addChildComponent(*audio_);
    }
    if (audio_) {
        audio_->setVisible(name == "Audio");
        if (name == "Audio")
            order_.push_back(audio_.get());
    }
    // One heading over the page's settings until the registry carries Live's sections.
    headings_.push_back(std::make_unique<SettingsHeading>(juce::String::fromUTF8(name.c_str())));
    content_.addAndMakeVisible(*headings_.back());
    order_.push_back(headings_.back().get());
    for (const auto &s : settings::registry()) {
        if (s.page != name)
            continue;
        // What the device chooser shows is not listed twice under it.
        if (audio_ && name == "Audio" &&
            (s.key == "audio.driverType" || s.key == "audio.inputDevice" || s.key == "audio.outputDevice" ||
             s.key == "audio.sampleRate" || s.key == "audio.bufferSize"))
            continue;
        rows_.push_back(std::make_unique<SettingRow>(*this, s));
        content_.addAndMakeVisible(*rows_.back());
        order_.push_back(rows_.back().get());
    }
    defaults.setEnabled(true);
    scroll_.setViewPosition(0, 0);
    layoutRows();
    pageList.repaint();
}
void SettingsPanel::find(const juce::String &query) {
    if (query.isEmpty()) {
        showPage(page_);
        return;
    }
    rows_.clear();
    headings_.clear();
    order_.clear();
    // The matches under their pages' headings, in the pages' order.
    std::string under;
    for (const auto *s : settings::search(query.toStdString())) {
        if (s->page != under) {
            under = s->page;
            headings_.push_back(std::make_unique<SettingsHeading>(juce::String::fromUTF8(under.c_str())));
            content_.addAndMakeVisible(*headings_.back());
            order_.push_back(headings_.back().get());
        }
        rows_.push_back(std::make_unique<SettingRow>(*this, *s));
        content_.addAndMakeVisible(*rows_.back());
        order_.push_back(rows_.back().get());
    }
    if (audio_)
        audio_->setVisible(false);
    // Restoring defaults is a page's; the results are no page.
    defaults.setEnabled(false);
    setHelp(rows_.empty() ? juce::String("Nothing matches.")
                          : juce::String(static_cast<int>(rows_.size())) +
                                (rows_.size() == 1 ? " setting matches." : " settings match."));
    scroll_.setViewPosition(0, 0);
    layoutRows();
    pageList.repaint();
}
void SettingsPanel::layoutRows() {
    const int width = std::max(0, scroll_.getWidth() - scroll_.getScrollBarThickness());
    int y = 0;
    for (auto *c : order_) {
        const int height = c == audio_.get()                                   ? audio_->getHeight()
                           : dynamic_cast<SettingsHeading *>(c) != nullptr ? SettingsHeading::kHeight
                                                                            : SettingRow::kHeight;
        c->setBounds(0, y, width, height);
        y += height;
    }
    content_.setSize(width, y + 8);
}
void SettingsPanel::refreshValues() {
    for (auto &row : rows_)
        row->refresh();
}
void SettingsPanel::setHelp(const juce::String &text) {
    if (text == help_)
        return;
    help_ = text;
    repaint(pageArea().withTrimmedTop(std::max(0, pageArea().getHeight() - kHelpHeight)));
}
bool SettingsPanel::change(const settings::Setting &s, const settings::Value &v) {
    // Applied at once and written at once: Live has no OK button (Master Reference V.1).
    std::string why;
    if (!store.set(s.key, v, {settings::Actor::User, {}, juce::Time::currentTimeMillis() * 1000}, why)) {
        setHelp(juce::String::fromUTF8(why.c_str()));
        return false;
    }
    // The value is in force from here, written or not.
    if (onChange)
        onChange();
    if (!store.save(why)) {
        setHelp("Not saved: " + juce::String::fromUTF8(why.c_str()));
        return false;
    }
    return true;
}
juce::Rectangle<int> SettingsPanel::pageArea() const noexcept {
    return {kPageListWidth, 6, std::max(0, getWidth() - kPageListWidth - 7), std::max(0, getHeight() - 12)};
}
void SettingsPanel::resized() {
    const auto page = pageArea();
    pageList.setBounds(0, 0, kPageListWidth, getHeight());
    // ADI's Find box at the page's head (R-02); Live has none.
    search.setBounds(page.getX() + SettingRow::kLabelX, page.getY() + 10,
                     std::max(0, page.getWidth() - 2 * SettingRow::kLabelX), 19);
    scroll_.setBounds(page.getX(), page.getY() + 36, page.getWidth(),
                      std::max(0, page.getHeight() - 36 - kHelpHeight));
    defaults.setBounds(page.getRight() - 133, page.getBottom() - 27, 120, 18);
    layoutRows();
}
void SettingsPanel::paint(juce::Graphics &g) {
    // Live's window: the frame's grey, the page a lighter panel with round corners.
    g.fillAll(look::frame);
    const auto page = pageArea();
    g.setColour(look::panel);
    g.fillRoundedRectangle(page.toFloat(), 6.f);
    // ADI's help line at the page's foot: what the setting under the mouse does (R-02).
    const juce::Rectangle<int> foot(page.getX() + SettingRow::kLabelX, page.getBottom() - kHelpHeight + 4,
                                    page.getWidth() - 2 * SettingRow::kLabelX, kHelpHeight - 36);
    g.setColour(look::outline);
    g.fillRect(foot.getX(), foot.getY() - 4, foot.getWidth(), 1);
    g.setColour(look::text);
    g.setFont(look::font(11.5f));
    g.drawFittedText(help_, foot.withTrimmedTop(2), juce::Justification::topLeft, 2, 1.f);
}
bool SettingsPanel::keyPressed(const juce::KeyPress &k) {
    if (k == juce::KeyPress::escapeKey) {
        if (onClose)
            onClose();
        return true;
    }
    // Ctrl+Tab (Option+Tab on the Mac) moves between the pages; Shift goes back
    // (Live 12 §40.1.2).
    const auto mods = k.getModifiers();
    if (k.getKeyCode() == juce::KeyPress::tabKey && (mods.isCtrlDown() || mods.isAltDown()) &&
        !pageNames_.empty()) {
        auto at = std::find(pageNames_.begin(), pageNames_.end(), page_);
        const auto n = static_cast<std::ptrdiff_t>(pageNames_.size());
        const auto i = at == pageNames_.end() ? 0 : at - pageNames_.begin();
        const auto next = (i + (mods.isShiftDown() ? n - 1 : 1)) % n;
        showPage(pageNames_[static_cast<std::size_t>(next)]);
        return true;
    }
    return false;
}
SettingsWindow::SettingsWindow(settings::AppSettings &store,
                               std::function<std::unique_ptr<juce::Component>()> audioDevices)
    : juce::DocumentWindow("Settings", look::panel, juce::DocumentWindow::closeButton, true) {
    setUsingNativeTitleBar(true);
    auto panel = std::make_unique<SettingsPanel>(store, std::move(audioDevices));
    panel->onClose = [this] { setVisible(false); };
    panel_ = panel.get();
    setContentOwned(panel.release(), false);
    setResizable(true, false);
    setResizeLimits(500, 560, 2000, 1600);
    // Live 11's Preferences open 500 by 607; ADI's has more pages and its Find box.
    centreWithSize(560, 680);
}
void SettingsWindow::closeButtonPressed() { setVisible(false); }
} // namespace adi::ui
