// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/adi_shaper_panel_session.hpp"
#include "adi/dsp/adi_shaper_wave.hpp"
#include "adi/ui/project_document.hpp"
#include "juce/device_model.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <charconv>
#include <cmath>
namespace adi::ui {
namespace {
using Bank = dsp::AdiShaperWaveBank;
constexpr auto uid = "org.adidaw.builtin.adi-shaper";
int numericId(std::string_view text) {
    int id = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? id : 0;
}
int word(double normalized) { return static_cast<int>(std::lround(normalized * Bank::wordMax)); }
bool validKey(AdiShaperPanel::Key k) {
    return k.stage >= 0 && k.stage < 11 && k.band >= 0 && k.band < 3 && k.lane >= 0 && k.lane < 2;
}
std::map<int, double> storedParameters(ProjectDocument& doc, std::int64_t device) {
    SQLite::Statement rows(doc.store().db(),
        "SELECT param_id, normalized_value FROM plugin_params WHERE device_id=?");
    rows.bind(1, device);
    std::map<int, double> result;
    while (rows.executeStep()) result.emplace(numericId(rows.getColumn(0).getString()), rows.getColumn(1).getDouble());
    return result;
}

}
AdiShaperPanelSession::AdiShaperPanelSession(ProjectDocument& doc, std::int64_t id)
    : document_(doc), id_(id), token_(doc.session().deviceRuntimeToken(id)) {
    const auto publication = doc.parameterFeed().current();
    for (const auto& device : *publication) if (device.id == id) { stamp_ = device.instance; break; }
    if (auto* live = instance()) {
        for (int i = 0; i < live->paramCount(); ++i) {
            const auto* d = live->paramAt(i);
            if (d) defaults_.emplace(numericId(d->id), d->defaultValue.normalized);
        }
        refresh();
    }
}
AdiShaperPanelSession::~AdiShaperPanelSession() { cancel(); }
device::DeviceInstance* AdiShaperPanelSession::instance() const {
    if (retired_) return nullptr;
    auto* live = document_.session().resolveDevice(token_);
    const auto publication = document_.parameterFeed().current();
    const bool same = stamp_ == 0 || std::any_of(publication->begin(), publication->end(),
        [&](const auto& device) { return device.id == id_ && device.instance == stamp_; });
    if (!live || live->identity().uid != uid || !same) { retired_ = true; return nullptr; }
    return live;
}
void AdiShaperPanelSession::refresh() {
    if (editing_) return;
    auto* live = instance();
    if (!live) { values_.clear(); return; }
    // The native built-in's single snapshot walks its target array once.
    const auto state = Payload::from_cbor(live->saveState("state"), true, false);
    if (!state.is_object() || !state.contains("params")) return;
    values_ = defaults_;
    for (const auto& [name, value] : state["params"].items()) {
        const int id = numericId(name);
        if (defaults_.contains(id) && value.is_number()) values_[id] = value.get<double>();
    }
}
AdiShaperPanel::Binding AdiShaperPanelSession::binding() {
    AdiShaperPanel::Binding b;
    b.parameterCount = defaults_.contains(416) ? 463 : 415;
    b.read = [this](int id) { const auto i = values_.find(id); return i == values_.end() ? 0.0 : i->second; };
    b.parameters = [this](Phase p, auto changes) { return parameters(p, changes); };
    if (defaults_.contains(Bank::firstId)) {
        b.wave = [this](Key k) { return wave(k); };
        b.editWave = [this](Key k, Phase p, auto points) { return editWave(k, p, points); };
    }
    return b;
}
void AdiShaperPanelSession::cancel() {
    if (!editing_) return;
    if (auto* live = instance()) for (const auto& [id, t] : touched_) {
        const auto key = std::to_string(id);
        if (live->getParam(key).normalized == t.after)
            live->setParam(key, device::ParamValue::fromNormalized(t.before));
    }
    editing_ = false; touched_.clear(); refresh();
}
bool AdiShaperPanelSession::parameters(Phase phase, std::span<const Change> changes) {
    if (phase == Phase::Cancel) { cancel(); return true; }
    auto* live = instance();
    if (!live) { cancel(); return false; }
    if (phase == Phase::Begin) {
        if (editing_) return false;
        error_.clear(); refresh();
        storedAtBegin_ = storedParameters(document_, id_);
        editing_ = true; return true;
    }
    if (!editing_) return false;
    for (auto [id, v] : changes) {
        if (!values_.contains(id) || !std::isfinite(v) || v < 0 || v > 1) { cancel(); return false; }
        const std::string name = std::to_string(id);
        const double previous = live->getParam(name).normalized;
        const auto previousEdit = touched_.find(id);
        if (previousEdit != touched_.end() && previous != previousEdit->second.after) {
            error_ = "The parameter changed outside this editor."; cancel(); return false;
        }
        if (static_cast<float>(previous) == static_cast<float>(v)) continue;
        auto [i, inserted] = touched_.try_emplace(id, Touched{previous, previous});
        if (inserted && !document_.session().overrideDeviceAutomation(id_, name)) { cancel(); return false; }
        if (instance() != live) { cancel(); return false; }
        if (!live->setParam(name, device::ParamValue::fromNormalized(v))) { cancel(); return false; }
        values_[id] = i->second.after = live->getParam(name).normalized;
    }
    if (phase != Phase::End) return true;
    bool changed = false;
    const auto storedNow = storedParameters(document_, id_);
    for (const auto& [id, t] : touched_) if (t.before != t.after) {
        const auto before = storedAtBegin_.find(id);
        const auto now = storedNow.find(id);
        const bool wasPresent = before != storedAtBegin_.end();
        if (wasPresent != (now != storedNow.end()) || (wasPresent && before->second != now->second) ||
            live->getParam(std::to_string(id)).normalized != t.after) {
            error_ = "The parameter changed outside this editor."; cancel(); return false;
        }
        changed = true;
    }
    if (changed) {
        // A curve may edit several words. The device's ordinary state-capture
        // boundary preserves all of them as one undoable preset-style edit.
        // ParamOps owns the first-touch values, blob and exact-instance replay.
        { const auto signal = live->beginStateSignal(); }
        document_.tick(static_cast<std::int64_t>(juce::Time::getMillisecondCounter()));
    }
    touched_.clear(); editing_ = false; return true;
}
std::vector<AdiShaperPanelSession::Point> AdiShaperPanelSession::wave(Key k) const {
    std::vector<Point> result;
    if (!validKey(k) || !values_.contains(Bank::lastId)) return result;
    const int curve = Bank::key(k.stage, k.band, k.lane);
    for (int i = Bank::firstId; i <= Bank::lastId; i += 2) {
        const int a = word(values_.at(i));
        if (Bank::curveOf(a) != curve) continue;
        auto p = Bank::unpack(a, word(values_.at(i + 1)));
        result.push_back({p.x, p.y, p.bend, static_cast<AdiShaperPanel::Shape>(p.shape)});
    }
    std::stable_sort(result.begin(), result.end(), [](Point a, Point b) { return a.x < b.x; });
    return result;
}
bool AdiShaperPanelSession::editWave(Key k, Phase phase, std::span<const Point> points) {
    if (phase == Phase::Begin || phase == Phase::Cancel) return parameters(phase, {});
    if (!editing_ || !validKey(k) || !values_.contains(Bank::lastId)) return false;
    const int target = Bank::key(k.stage, k.band, k.lane);
    std::vector<int> used, empty;
    for (int id = Bank::firstId; id <= Bank::lastId; id += 2) {
        const int a = word(values_.at(id));
        if (Bank::curveOf(a) == target) used.push_back(id);
        else if (a == 0) empty.push_back(id);
    }
    if (points.size() > used.size() + empty.size()) {
        error_ = "The device's 512 curve points are in use."; cancel(); return false;
    }
    if (points.size() > used.size()) {
        auto needed = points.size() - used.size();
        used.insert(used.end(), empty.begin(), empty.begin() + static_cast<std::ptrdiff_t>(needed));
        std::sort(used.begin(), used.end());
    }
    std::vector<Change> changes;
    for (std::size_t j = 0; j < used.size(); ++j) {
        int a = 0, v = 0;
        if (j < points.size()) {
            const auto& src = points[j];
            if (!std::isfinite(src.x) || !std::isfinite(src.y) || !std::isfinite(src.bend)) { cancel(); return false; }
            const Bank::Point p{src.x, src.y, src.bend, static_cast<Bank::Shape>(src.shape)};
            a = Bank::address(target, p); v = Bank::value(p);
        }
        const int id = used[j];
        if (word(values_.at(id)) != a) changes.push_back({id, double(a) / Bank::wordMax});
        if (word(values_.at(id + 1)) != v) changes.push_back({id + 1, double(v) / Bank::wordMax});
    }
    return parameters(phase, changes);
}
bool AdiShaperPanelSession::applyPreset(std::span<const std::uint8_t> bytes) {
    if (editing_ || bytes.empty() || bytes.size() > 1024 * 1024) return false;
    const auto s = Payload::from_cbor(bytes.begin(), bytes.end(), true, false);
    if (!s.is_object() || !s.contains("version") || s["version"] != 1 ||
        !s.contains("uid") || s["uid"] != uid || !s.contains("params") || !s["params"].is_object()) return false;
    auto target = defaults_;
    for (const auto& [name, v] : s["params"].items()) {
        const int id = numericId(name);
        if (!target.contains(id) || !v.is_number()) return false;
        const double n = v.get<double>();
        if (!std::isfinite(n) || n < 0 || n > 1) return false;
        target[id] = n;
    }
    if (!parameters(Phase::Begin, {})) return false;
    std::vector<Change> changes;
    for (const auto& [id, n] : target) if (n != values_.at(id)) changes.push_back({id, n});
    return parameters(Phase::End, changes);
}
} // namespace adi::ui
