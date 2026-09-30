// SPDX-License-Identifier: GPL-3.0-or-later
#include "microtuner.hpp"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <sstream>
namespace adi::device {
Microtuner::Microtuner() {
    const char *ids[]{"morph", "transpose", "reference"};
    const double lo[]{0, -48, 220}, hi[]{1, 48, 880}, def[]{0, 0, 440};
    for (std::size_t i = 0; i < 3; ++i) {
        auto &p = params_[i];
        p.id = ids[i];
        p.name = ids[i];
        p.domain = ParamDomain::Real;
        p.shape = ParamShape::Continuous;
        p.hasRealRange = true;
        p.minReal = lo[i];
        p.maxReal = hi[i];
        p.defaultValue = ParamValue::withReal((def[i] - lo[i]) / (hi[i] - lo[i]), def[i]);
        values_[i].store(p.defaultValue.normalized);
    }
    publish();
}
void Microtuner::publish() {
    auto t = std::make_unique<Tuning>();
    t->decks = tuning_.decks;
    publisher_.publish(std::move(t));
    publisher_.collect();
}
void Microtuner::prepare(double, std::int32_t) { notes_ = {}; }
const ParamDescriptor *Microtuner::paramAt(std::int32_t i) const noexcept {
    return i >= 0 && i < 3 ? &params_[static_cast<std::size_t>(i)] : nullptr;
}
double Microtuner::real(std::size_t i, double n) const noexcept {
    return params_[i].minReal + std::clamp(n, 0., 1.) * (params_[i].maxReal - params_[i].minReal);
}
ParamValue Microtuner::getParam(const std::string &id) const noexcept {
    for (std::size_t i = 0; i < 3; ++i)
        if (params_[i].id == id) {
            const auto n = values_[i].load(std::memory_order_relaxed);
            return ParamValue::withReal(n, real(i, n));
        }
    return {};
}
bool Microtuner::setParam(const std::string &id, const ParamValue &v) {
    if (!std::isfinite(v.normalized))
        return false;
    for (std::size_t i = 0; i < 3; ++i)
        if (params_[i].id == id) {
            values_[i].store(std::clamp(v.normalized, 0., 1.), std::memory_order_relaxed);
            return true;
        }
    return false;
}
bool Microtuner::setDeck(int deck, const std::array<double, 128> &offsets) {
    if (deck < 0 || deck > 1)
        return false;
    for (double v : offsets)
        if (!std::isfinite(v) || std::abs(v) > 1536)
            return false;
    tuning_.decks[static_cast<std::size_t>(deck)] = offsets;
    publish();
    return true;
}
bool Microtuner::generate(int deck, int divisions, double ratio, int root) {
    if (divisions < 1 || divisions > 128 || !std::isfinite(ratio) || ratio <= 1 || ratio > 16 ||
        root < 0 || root > 127)
        return false;
    std::array<double, 128> offsets{};
    for (std::size_t i = 0; i < 128; ++i)
        offsets[i] = (static_cast<double>(i) - root) * (12 * std::log2(ratio) / divisions - 1);
    return setDeck(deck, offsets);
}
bool Microtuner::importScala(int deck, const std::string &text, int root) {
    if (text.size() > 65536 || root < 0 || root > 127)
        return false;
    try {
        std::istringstream input(text);
        input.imbue(std::locale::classic());
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line)) {
            line = line.substr(0, line.find('!'));
            auto a = line.find_first_not_of(" \t\r");
            if (a == std::string::npos)
                continue;
            auto b = line.find_last_not_of(" \t\r");
            lines.push_back(line.substr(a, b - a + 1));
        }
        if (lines.size() < 3)
            return false;
        std::size_t used = 0;
        const int count = std::stoi(lines[1], &used);
        if (used != lines[1].size() || count < 1 || count > 128 ||
            lines.size() != static_cast<std::size_t>(count + 2))
            return false;
        std::vector<double> cents{0};
        auto number = [](const std::string &value) {
            std::size_t n = 0;
            double x = std::stod(value, &n);
            if (n != value.size() || !std::isfinite(x))
                throw std::invalid_argument("number");
            return x;
        };
        for (int i = 0; i < count; ++i) {
            const auto &token = lines[static_cast<std::size_t>(i + 2)];
            const auto slash = token.find('/');
            double value = 0;
            if (slash != std::string::npos) {
                const auto num = number(token.substr(0, slash)),
                           den = number(token.substr(slash + 1));
                if (num <= 0 || den <= 0)
                    return false;
                value = 1200 * std::log2(num / den);
            } else if (token.find('.') != std::string::npos)
                value = number(token);
            else {
                const auto ratio = number(token);
                if (ratio <= 0)
                    return false;
                value = 1200 * std::log2(ratio);
            }
            if (!std::isfinite(value) || value <= cents.back() || value > 4800)
                return false;
            cents.push_back(value);
        }
        std::array<double, 128> offsets{};
        for (int key = 0; key < 128; ++key) {
            const int degree = key - root;
            const int period = static_cast<int>(std::floor(static_cast<double>(degree) / count));
            const auto index = static_cast<std::size_t>(degree - period * count);
            offsets[static_cast<std::size_t>(key)] =
                (period * cents.back() + cents[index]) / 100 - degree;
        }
        return setDeck(deck, offsets);
    } catch (const std::exception &) {
        return false;
    }
}
double Microtuner::offset(const Tuning &t, std::uint16_t key) const noexcept {
    const double morph = values_[0].load(std::memory_order_relaxed);
    return t.decks[0][key] * (1 - morph) + t.decks[1][key] * morph +
           real(1, values_[1].load(std::memory_order_relaxed)) +
           12 * std::log2(real(2, values_[2].load(std::memory_order_relaxed)) / 440);
}
void Microtuner::transformEvents(engine::EventSpan inputEvents, engine::EventList &out,
                                 std::int32_t, double, const engine::TransportInfo *) noexcept {
    engine::SnapshotPublisher<Tuning>::AudioRead read(publisher_);
    if (!read.valid())
        return;
    auto pass = [&](const engine::Event &e) {
        if (out.push(e))
            return true;
        if (e.owner && (e.type == engine::EventType::NoteOff ||
                        (e.type == engine::EventType::NoteOn && e.value == 0)))
            e.owner->noteOffRejected(e);
        return false;
    };
    const auto count = inputEvents.count;
    auto tune = [&](Note &n, int frame) {
        const double pitch = offset(*read.get(), n.source.dim) + n.expression;
        if (pitch == n.last)
            return;
        auto e = n.source;
        e.type = engine::EventType::NoteExpression;
        e.dim = 0;
        e.frame = frame;
        e.value = pitch;
        if (out.push(e))
            n.last = pitch;
    };
    for (auto &n : notes_)
        if (n.active)
            tune(n, 0);
    for (int i = 0; i < count; ++i) {
        auto e = inputEvents.first[i];
        if (e.type == engine::EventType::ParamValue && e.paramId < 3 && std::isfinite(e.value)) {
            values_[e.paramId].store(std::clamp(e.value, 0., 1.), std::memory_order_relaxed);
            out.push(e);
            for (auto &n : notes_)
                if (n.active)
                    tune(n, e.frame);
            continue;
        }
        if (e.type == engine::EventType::NoteOn && e.dim < 128 && e.value > 0) {
            auto it =
                std::find_if(notes_.begin(), notes_.end(), [](const Note &n) { return !n.active; });
            if (it == notes_.end())
                continue;
            if (!out.push(e))
                continue;
            *it = {true, e, 0, std::numeric_limits<double>::quiet_NaN()};
            tune(*it, e.frame);
            continue;
        }
        auto it = std::find_if(notes_.begin(), notes_.end(), [&](const Note &n) {
            return n.active && n.source.channel == e.channel &&
                   (e.noteId ? n.source.noteId == e.noteId : n.source.dim == e.dim);
        });
        if (it != notes_.end() && e.type == engine::EventType::NoteExpression && e.dim == 0) {
            if (!std::isfinite(e.value))
                continue;
            it->expression = e.value;
            tune(*it, e.frame);
            continue;
        }
        if (pass(e) && it != notes_.end() &&
            (e.type == engine::EventType::NoteOff ||
             (e.type == engine::EventType::NoteOn && e.value == 0)))
            it->active = false;
    }
    out.sortByFrame();
}
void Microtuner::process(const engine::NodeIo &io) noexcept {
    for (int c = 0; c < io.channels; ++c)
        if (io.out && io.out[c]) {
            if (io.in && io.in[c])
                std::copy_n(io.in[c] + io.blockOffset, io.frames, io.out[c] + io.blockOffset);
            else
                std::fill_n(io.out[c] + io.blockOffset, io.frames, 0.f);
        }
}
std::vector<std::uint8_t> Microtuner::saveState(const std::string &role) const {
    if (role != "component")
        return {};
    nlohmann::json j = {
        {"version", 1}, {"decks", tuning_.decks}, {"params", nlohmann::json::array()}};
    for (const auto &v : values_)
        j["params"].push_back(v.load(std::memory_order_relaxed));
    return nlohmann::json::to_cbor(j);
}
bool Microtuner::loadState(const std::string &role, const std::vector<std::uint8_t> &bytes) {
    if (role != "component" || bytes.size() > 65536)
        return false;
    try {
        const auto j = nlohmann::json::from_cbor(bytes);
        if (j.at("version") != 1)
            return false;
        const auto decks = j.at("decks").get<std::array<std::array<double, 128>, 2>>();
        const auto values = j.at("params").get<std::array<double, 3>>();
        for (const auto &deck : decks)
            for (double v : deck)
                if (!std::isfinite(v) || std::abs(v) > 1536)
                    return false;
        for (double v : values)
            if (!std::isfinite(v) || v < 0 || v > 1)
                return false;
        tuning_.decks = decks;
        for (std::size_t i = 0; i < 3; ++i)
            values_[i].store(values[i], std::memory_order_relaxed);
        publish();
        return true;
    } catch (const nlohmann::json::exception &) {
        return false;
    }
}
} // namespace adi::device
