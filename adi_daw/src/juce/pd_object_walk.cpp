// SPDX-License-Identifier: GPL-3.0-or-later

#include "juce/pd_object_walk.hpp"

#include "juce/pd_declarations.hpp"

#include <cstdlib>
#include <utility>

namespace adi::device {

const char* toString(PdObjectRefusal r) noexcept {
    switch (r) {
        case PdObjectRefusal::Unknown: return "pd.unknown_object";
        case PdObjectRefusal::DynamicClass: return "pd.dynamic_class";
        case PdObjectRefusal::Recursive: return "pd.recursive_abstraction";
        case PdObjectRefusal::TooDeep: return "pd.too_deep";
        case PdObjectRefusal::Malformed: return "pd.malformed_canvas";
    }
    return "pd.unknown_object";
}

std::vector<std::string> PdObjectWalk::describe() const {
    std::vector<std::string> out;
    for (const auto& r : refused) {
        std::string line;
        switch (r.reason) {
            case PdObjectRefusal::Unknown: line = "unknown object '" + r.className + "'"; break;
            case PdObjectRefusal::DynamicClass:
                line = "class '" + r.className + "' is only known when the patch runs";
                break;
            case PdObjectRefusal::Recursive: line = "abstraction '" + r.className + "' contains itself"; break;
            case PdObjectRefusal::TooDeep: line = "'" + r.className + "' is nested too deep"; break;
            case PdObjectRefusal::Malformed: line = "a subpatch is not closed properly"; break;
        }
        line += r.file.empty() ? " in the patch" : " in abstraction '" + r.file + "'";
        if (!r.subpatch.empty()) line += ", " + r.subpatch;
        if (r.box > 0) line += ", box " + std::to_string(r.box);
        out.push_back(std::move(line));
    }
    return out;
}

namespace {

bool isNumber(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0';
}

/// Pd 0.56's `clone_new`: flags first (-s <n>, -x, -d, -di, -do), then either
/// `<n> <name>` or `<name> <n>`. Empty when no name can be read.
std::string cloneTarget(const std::vector<std::string>& atoms) {
    std::size_t i = 5;
    while (i < atoms.size() && !atoms[i].empty() && atoms[i][0] == '-' && !isNumber(atoms[i])) {
        if (atoms[i] == "-s" && i + 1 < atoms.size() && isNumber(atoms[i + 1])) i += 2;
        else if (atoms[i] == "-x" || atoms[i] == "-d" || atoms[i] == "-di" || atoms[i] == "-do") ++i;
        else break;
    }
    if (i + 1 >= atoms.size()) return {};
    const std::string& a = atoms[i];
    const std::string& b = atoms[i + 1];
    // Pd takes atom 1 as the name when atom 0 reads as a count of zero or
    // more (a symbol reads as 0) and atom 1 is a symbol; else atom 0 when it
    // is a symbol and atom 1 a count.
    const bool aCount = !isNumber(a) || std::strtod(a.c_str(), nullptr) >= 0.0;
    if (aCount && !isNumber(b)) return b;
    if (!isNumber(a) && isNumber(b) && std::strtod(b.c_str(), nullptr) >= 0.0) return a;
    return {};
}

class Walker {
public:
    Walker(const PdAbstractions& abstractions, const PdVocabulary& vocabulary)
        : abstractions_(abstractions), vocabulary_(vocabulary) {}

    void file(std::string_view text, const std::string& name, int depth) {
        const auto records = pdRecords(text);

        // Pass 1: each subpatch's name is on its `#X restore`, which comes
        // after its contents, so pair canvases with restores first.
        std::vector<std::string> canvasName(records.size());
        {
            std::vector<std::size_t> open;
            for (std::size_t i = 0; i < records.size(); ++i) {
                const auto& r = records[i];
                if (r.size() >= 2 && r[0] == "#N" && r[1] == "canvas") open.push_back(i);
                if (r.size() >= 2 && r[0] == "#X" && r[1] == "restore") {
                    if (open.size() <= 1) {
                        refuse(PdObjectRefusal::Malformed, "", name, "", 0);
                        return;
                    }
                    std::string label;
                    for (std::size_t k = 4; k < r.size(); ++k) label += (k > 4 ? " " : "") + r[k];
                    canvasName[open.back()] = label.empty() ? "subpatch" : label;
                    open.pop_back();
                }
            }
            if (open.size() > 1) {
                refuse(PdObjectRefusal::Malformed, "", name, "", 0);
                return;
            }
        }

        // Pass 2: the objects, with the subpatch path they sit in.
        std::vector<std::string> path;
        bool root = true;
        int box = 0;
        for (std::size_t i = 0; i < records.size(); ++i) {
            const auto& r = records[i];
            if (r.size() >= 2 && r[0] == "#N" && r[1] == "canvas") {
                if (root) { root = false; continue; }
                path.push_back(canvasName[i]);
                continue;
            }
            if (r.size() >= 2 && r[0] == "#X" && r[1] == "restore") {
                if (!path.empty()) path.pop_back();
                continue;
            }
            if (r.size() < 5 || r[0] != "#X" || r[1] != "obj") continue;
            ++box;
            ++objects_;
            const int here = depth + static_cast<int>(path.size());
            object(r, name, joined(path), box, here);
        }
    }

    PdObjectWalk result() && {
        PdObjectWalk w;
        w.refused = std::move(refused_);
        w.abstractions = std::move(used_);
        w.objects = objects_;
        return w;
    }

private:
    static std::string joined(const std::vector<std::string>& path) {
        std::string s;
        for (const auto& p : path) s += (s.empty() ? "" : "/") + p;
        return s;
    }

    void refuse(PdObjectRefusal why, std::string cls, const std::string& file,
                std::string subpatch, int box) {
        refused_.push_back(PdRefusedObject{why, std::move(cls), file, std::move(subpatch), box});
    }

    /// The stored abstraction a class names, or end().
    PdAbstractions::const_iterator findAbstraction(const std::string& cls) const {
        auto it = abstractions_.find(cls);
        if (it == abstractions_.end()) it = abstractions_.find(cls + ".pd");
        return it;
    }

    void object(const std::vector<std::string>& r, const std::string& file,
                const std::string& subpatch, int box, int depth) {
        const std::string& cls = r[4];
        if (depth > kPdWalkMaxDepth) {
            refuse(PdObjectRefusal::TooDeep, cls, file, subpatch, box);
            return;
        }
        if (cls.find('$') != std::string::npos) {
            refuse(PdObjectRefusal::DynamicClass, cls, file, subpatch, box);
            return;
        }
        if (vocabulary_.contains(cls)) {
            if (cls == "clone") {
                const std::string target = cloneTarget(r);
                if (target.find('$') != std::string::npos)
                    refuse(PdObjectRefusal::DynamicClass, target, file, subpatch, box);
                else if (target.empty() || !abstraction(target, file, subpatch, box, depth))
                    refuse(PdObjectRefusal::Unknown, target.empty() ? "clone" : target, file,
                           subpatch, box);
            }
            return;
        }
        if (!abstraction(cls, file, subpatch, box, depth))
            refuse(PdObjectRefusal::Unknown, cls, file, subpatch, box);
    }

    /// Walks a stored abstraction once. False when none is stored by that name.
    bool abstraction(const std::string& cls, const std::string& file, const std::string& subpatch,
                     int box, int depth) {
        const auto it = findAbstraction(cls);
        if (it == abstractions_.end()) return false;
        const std::string& key = it->first;
        if (inProgress_.count(key) != 0) {
            refuse(PdObjectRefusal::Recursive, key, file, subpatch, box);
            return true;
        }
        if (done_.count(key) != 0) return true;
        if (depth + 1 > kPdWalkMaxDepth) {
            refuse(PdObjectRefusal::TooDeep, key, file, subpatch, box);
            return true;
        }
        used_.push_back(key);
        inProgress_.insert(key);
        this->file(it->second, key, depth + 1);
        inProgress_.erase(key);
        done_.insert(key);
        return true;
    }

    const PdAbstractions& abstractions_;
    const PdVocabulary& vocabulary_;
    std::vector<PdRefusedObject> refused_;
    std::vector<std::string> used_;
    std::set<std::string, std::less<>> inProgress_, done_;
    std::size_t objects_ = 0;
};

}  // namespace

PdObjectWalk walkPdObjects(std::string_view patchText, const PdAbstractions& abstractions,
                           const PdVocabulary& vocabulary) {
    Walker w(abstractions, vocabulary);
    w.file(patchText, "", 0);
    return std::move(w).result();
}

}  // namespace adi::device
