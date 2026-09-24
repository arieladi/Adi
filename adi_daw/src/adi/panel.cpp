// SPDX-License-Identifier: GPL-3.0-or-later
//
// See panel.hpp. ADR-0150, ADR-0154.

#include "adi/panel.hpp"

#include <algorithm>
#include <cctype>

namespace adi::panel {

namespace {

// ASCII case folding. Parameter names are overwhelmingly ASCII, and a byte
// that is not ASCII is compared as it is, so a Hebrew or accented name still
// matches itself exactly; full Unicode folding is the library index's job.
std::string fold(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (static_cast<unsigned char>(c) < 0x80) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> words(std::string_view q) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : q) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) out.push_back(fold(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(fold(cur));
    return out;
}

bool startsWord(const std::string& hay, std::size_t pos) {
    if (pos == 0) return true;
    const auto prev = static_cast<unsigned char>(hay[pos - 1]);
    return !std::isalnum(prev) || (std::isdigit(prev) != 0) != (std::isdigit(static_cast<unsigned char>(hay[pos])) != 0);
}

/// The best place `w` occurs in `hay`: 3 at a word start, 1 inside a word,
/// 0 absent.
int wordScore(const std::string& hay, const std::string& w) {
    int best = 0;
    for (std::size_t pos = hay.find(w); pos != std::string::npos; pos = hay.find(w, pos + 1)) {
        best = std::max(best, startsWord(hay, pos) ? 3 : 1);
        if (best == 3) break;
    }
    return best;
}

}  // namespace

Resolved resolve(const rows::Model& model, std::int64_t deviceId,
                 const std::vector<Declared>& declared) {
    Resolved r;
    for (const Declared& d : declared)
        if (d.modifiable) ++r.modifiableCount;

    for (const rows::DevicePanel& p : model.devicePanels) {
        if (p.deviceId != deviceId) continue;
        r.configured = true;
        for (const std::string& id : p.params) {
            Entry e;
            e.id = id;
            e.missing = std::none_of(declared.begin(), declared.end(),
                                     [&](const Declared& d) { return d.id == id; });
            r.entries.push_back(std::move(e));
        }
        return r;
    }

    // Live's default. Nothing declared (a placeholder) is an empty panel
    // with no prompt: there is nothing to configure it from.
    if (r.modifiableCount == 0) return r;
    if (r.modifiableCount > kDefaultLimit) {
        r.showsConfigureHint = true;
        return r;
    }
    for (const Declared& d : declared)
        if (d.modifiable) r.entries.push_back({d.id, false});
    return r;
}

std::vector<Match> search(const std::vector<Declared>& declared, std::string_view query,
                          std::size_t limit) {
    const std::vector<std::string> ws = words(query);
    const std::string whole = fold(query);
    std::vector<Match> out;
    for (std::size_t i = 0; i < declared.size(); ++i) {
        const std::string name = fold(declared[i].name);
        const std::string id = fold(declared[i].id);
        // The id matches only as the WHOLE query: VST3 ids are bare numbers,
        // and a word like the "3" in "filter 3" must not find the parameter
        // whose id happens to be 3.
        if (!ws.empty() && id == whole) {
            out.push_back({i, 50});
            continue;
        }
        int score = 0;
        bool all = true;
        for (const std::string& w : ws) {
            const int ws1 = wordScore(name, w);
            if (ws1 == 0) { all = false; break; }
            score += ws1;
        }
        if (!all) continue;
        if (!ws.empty() && name == whole) score += 100;              // the exact name
        else if (!ws.empty() && name.rfind(whole, 0) == 0) score += 10;   // the name starts with it
        out.push_back({i, score});
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Match& a, const Match& b) { return a.score > b.score; });
    if (limit != 0 && out.size() > limit) out.resize(limit);
    return out;
}

}  // namespace adi::panel
