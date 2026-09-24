// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/settings/bundle.hpp"

#include "adi/media/zip_writer.hpp"

#include <miniz.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>

namespace adi::settings {
namespace {

constexpr const char* kRolePrefix = "role:";
constexpr const char* kKind = "adi-settings-bundle";

std::string generic(const std::filesystem::path& p) {
    return p.lexically_normal().generic_string();
}

bool looksAbsolute(const std::string& s) {
    if (s.empty()) return false;
    if (s[0] == '/' || s[0] == '\\') return true;   // POSIX, and UNC or rooted Windows
    return s.size() >= 3 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':' &&
           (s[2] == '\\' || s[2] == '/');           // C:\ or C:/
}

bool writeJson(const std::filesystem::path& p, const Value& v) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << v.dump(2) << '\n';
    return static_cast<bool>(out);
}

/// A whole small archive, in memory: bundles are kilobytes. miniz is built
/// without stdio here (ADR-0127), so the file is read by a stream first.
class ZipReader {
public:
    explicit ZipReader(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return;
        bytes_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        ok_ = !bytes_.empty() &&
              mz_zip_reader_init_mem(&zip_, bytes_.data(), bytes_.size(), 0) != 0;
    }
    ~ZipReader() {
        if (ok_) mz_zip_reader_end(&zip_);
    }
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }

    std::optional<std::string> read(const std::string& name) {
        if (!ok_) return std::nullopt;
        const int index = mz_zip_reader_locate_file(&zip_, name.c_str(), nullptr, 0);
        if (index < 0) return std::nullopt;
        std::size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip_, static_cast<mz_uint>(index), &size, 0);
        if (!data) return std::nullopt;
        std::string out(static_cast<const char*>(data), size);
        mz_free(data);
        return out;
    }

    std::vector<std::string> names() {
        std::vector<std::string> out;
        if (!ok_) return out;
        const mz_uint n = mz_zip_reader_get_num_files(&zip_);
        for (mz_uint i = 0; i < n; ++i) {
            char name[512];
            if (mz_zip_reader_get_filename(&zip_, i, name, static_cast<mz_uint>(sizeof name)) > 0) out.emplace_back(name);
        }
        return out;
    }

private:
    std::string bytes_;
    mz_zip_archive zip_{};
    bool ok_ = false;
};

}  // namespace

Roles rolesOf(const AppSettings& s) {
    Roles r;
    const auto add = [&](const std::string& name, const Value& v) {
        if (v.is_string() && !v.get<std::string>().empty()) r[name] = v.get<std::string>();
    };
    // Roles are the ROOTS a user's material lives under -- the user library
    // and the content folders -- never a folder that is itself a setting to
    // be carried: the record folder travels as a path UNDER a root.
    add("user library", s.get("library.userLibrary"));
    const Value folders = s.get("library.contentFolders");
    for (std::size_t i = 0; i < folders.size(); ++i)
        add("content folder " + std::to_string(i + 1), folders[i]);
    return r;
}

std::optional<std::string> toRole(const std::filesystem::path& p, const Roles& roles) {
    const std::string target = generic(p);
    const std::string* bestName = nullptr;
    std::size_t bestLen = 0;
    for (const auto& [name, root] : roles) {
        std::string base = generic(root);
        while (base.size() > 1 && base.back() == '/') base.pop_back();
        const bool under = target == base ||
                           (target.size() > base.size() && target.compare(0, base.size(), base) == 0 &&
                            target[base.size()] == '/');
        if (under && base.size() > bestLen) {
            bestLen = base.size();
            bestName = &name;
        }
    }
    if (!bestName) return std::nullopt;
    std::string rest = target.substr(bestLen);   // "" or "/sub/dir"
    return std::string(kRolePrefix) + *bestName + rest;
}

std::optional<std::filesystem::path> fromRole(const std::string& s, const Roles& roles) {
    if (s.rfind(kRolePrefix, 0) != 0) return std::nullopt;
    const std::string body = s.substr(std::char_traits<char>::length(kRolePrefix));
    // The longest role name that prefixes the body, followed by '/' or the end.
    const std::string* bestName = nullptr;
    for (const auto& [name, root] : roles) {
        (void)root;
        const bool match = body == name ||
                           (body.size() > name.size() && body.compare(0, name.size(), name) == 0 &&
                            body[name.size()] == '/');
        if (match && (!bestName || name.size() > bestName->size())) bestName = &name;
    }
    if (!bestName) return std::nullopt;
    std::filesystem::path out = roles.at(*bestName);
    if (body.size() > bestName->size()) out /= std::filesystem::path(body.substr(bestName->size() + 1));
    return out.lexically_normal();
}

bool containsAbsolutePath(const Value& v) {
    if (v.is_string()) return looksAbsolute(v.get<std::string>());
    if (v.is_array() || v.is_object()) {
        for (const auto& e : v)
            if (containsAbsolutePath(e)) return true;
    }
    return false;
}

BundleReport exportBundle(const AppSettings& store, const std::vector<Preset>& presets,
                          const std::filesystem::path& zipFile) {
    BundleReport rep;
    const Roles roles = rolesOf(store);

    // A path value: every path as a role, or left out and reported.
    const auto asRoles = [&](const Setting& s, const Value& v, Value& out) {
        if (s.type == Type::Path) {
            const std::string p = v.get<std::string>();
            if (p.empty()) { out = p; return true; }
            if (const auto r = toRole(p, roles)) { out = *r; return true; }
            rep.leftOut.push_back(s.key + ": " + p);
            return false;
        }
        out = Value::array();
        for (const auto& e : v) {
            if (const auto r = toRole(e.get<std::string>(), roles)) out.push_back(*r);
            else rep.leftOut.push_back(s.key + ": " + e.get<std::string>());
        }
        return true;
    };

    const auto portable = [&](const Value& values) {
        Value out = Value::object();
        for (const auto& [key, v] : values.items()) {
            const Setting* s = find(key);
            if (!s || s->scope != Scope::App) continue;   // unknown or not ours: not exported
            std::string why;
            if (!validate(*s, v, why)) continue;
            if (s->type == Type::Path || s->type == Type::PathList) {
                Value converted;
                if (asRoles(*s, v, converted)) out[key] = converted;
            } else {
                out[key] = v;
            }
        }
        return out;
    };

    Value settingsDoc = {{"schema", kSettingsSchema},
                         {"app", appdata::displayName(store.app())},
                         {"values", portable(store.stored())}};
    std::vector<Value> presetDocs;
    for (const auto& p : presets) {
        Preset copy = p;
        copy.values = portable(p.values);
        presetDocs.push_back(toJson(copy));
    }

    // The guarantee, checked on what is about to be written rather than
    // trusted from the conversion above.
    bool leak = containsAbsolutePath(settingsDoc);
    for (const auto& d : presetDocs) leak = leak || containsAbsolutePath(d);
    if (leak) {
        rep.why = "an absolute path would have left this machine in the bundle";
        return rep;
    }

    std::error_code ec;
    auto stage = zipFile;
    stage += ".staging";
    std::filesystem::remove_all(stage, ec);
    std::filesystem::create_directories(stage / "presets", ec);
    const Value manifest = {{"kind", kKind},
                            {"schema", kSettingsSchema},
                            {"app", appdata::displayName(store.app())}};
    bool ok = writeJson(stage / "manifest.json", manifest) &&
              writeJson(stage / "settings.json", settingsDoc);
    for (std::size_t i = 0; ok && i < presetDocs.size(); ++i)
        ok = writeJson(stage / "presets" / (std::to_string(i + 1) + ".json"), presetDocs[i]);

    if (ok) {
        media::ZipWriter zip(zipFile);
        ok = zip.addFile(stage / "manifest.json", "manifest.json") &&
             zip.addFile(stage / "settings.json", "settings.json");
        for (std::size_t i = 0; ok && i < presetDocs.size(); ++i) {
            const std::string name = std::to_string(i + 1) + ".json";
            ok = zip.addFile(stage / "presets" / name, "presets/" + name);
        }
        ok = ok && zip.finish();
    }
    std::filesystem::remove_all(stage, ec);
    if (!ok) {
        std::filesystem::remove(zipFile, ec);
        rep.why = "could not write " + zipFile.string();
        return rep;
    }
    rep.ok = true;
    return rep;
}

BundleReport importBundle(const std::filesystem::path& zipFile, AppSettings& target,
                          const ChangeContext& who) {
    BundleReport rep;
    ZipReader zip(zipFile);
    if (!zip.ok()) { rep.why = "not a readable bundle: " + zipFile.string(); return rep; }

    const auto parse = [](const std::optional<std::string>& text) {
        return text ? Value::parse(*text, nullptr, false) : Value(Value::value_t::discarded);
    };
    const Value manifest = parse(zip.read("manifest.json"));
    if (manifest.is_discarded() || !manifest.is_object() || manifest.value("kind", "") != kKind) {
        rep.why = "no settings-bundle manifest";
        return rep;
    }
    if (manifest.value("app", "") != appdata::displayName(target.app())) {
        rep.why = "this bundle is " + manifest.value("app", std::string("unnamed")) +
                  "'s; settings do not cross applications (ADR-0145 d10)";
        return rep;
    }
    const Value settingsDoc = parse(zip.read("settings.json"));
    if (settingsDoc.is_discarded() || !settingsDoc.contains("values") ||
        !settingsDoc.at("values").is_object()) {
        rep.why = "the bundle's settings.json is unreadable";
        return rep;
    }

    const Roles roles = rolesOf(target);   // THIS machine's folders
    const auto localise = [&](const Setting& s, const Value& v, Value& out) {
        const auto one = [&](const Value& e, Value& dst) {
            if (!e.is_string()) return false;
            const std::string str = e.get<std::string>();
            if (str.empty()) { dst = str; return true; }
            if (const auto p = fromRole(str, roles)) { dst = p->generic_string(); return true; }
            rep.unresolved.push_back(s.key + ": " + str);
            return false;
        };
        if (s.type == Type::Path) return one(v, out);
        out = Value::array();
        if (!v.is_array()) return false;
        for (const auto& e : v) {
            Value d;
            if (one(e, d)) out.push_back(d);
        }
        return true;
    };

    const auto bring = [&](const Value& values) {
        Value out = Value::object();
        for (const auto& [key, v] : values.items()) {
            const Setting* s = find(key);
            if (!s || s->scope != Scope::App) { rep.skipped.push_back(key); continue; }
            if (s->type == Type::Path || s->type == Type::PathList) {
                Value local;
                if (localise(*s, v, local)) out[key] = local;
            } else {
                out[key] = v;
            }
        }
        return out;
    };

    // Named, not iterated as a temporary: items() refers into the object,
    // and a range-for over a temporary's items() keeps only the proxy alive.
    const Value incoming = bring(settingsDoc.at("values"));
    for (const auto& [key, v] : incoming.items()) {
        std::string why;
        if (target.set(key, v, who, why)) ++rep.applied;
        else rep.skipped.push_back(key);
    }
    for (const auto& name : zip.names()) {
        if (name.rfind("presets/", 0) != 0) continue;
        std::string why;
        if (auto p = presetFromJson(parse(zip.read(name)), why)) {
            p->values = bring(p->values);
            rep.presets.push_back(std::move(*p));
        }
    }
    rep.ok = true;
    return rep;
}

}  // namespace adi::settings
