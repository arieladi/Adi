// SPDX-License-Identifier: GPL-3.0-or-later
//
// See pd_builtins.hpp for why registration happens once, in instance 0.

#include "juce/pd_builtins.hpp"

#include <cstring>
#include <mutex>
#include <vector>

namespace adi::device {
namespace {

/// The table. A FUNCTION-LOCAL static, not a namespace-scope one, and that is
/// the whole reason self-registration is safe here: a registrar in another
/// translation unit may run before this one's statics would have been
/// constructed, and a function-local static is constructed on first use --
/// which is that call. A namespace-scope vector would be a static
/// initialisation order fiasco with no symptom but a missing external.
struct Table {
    std::mutex mutex;
    std::vector<PdBuiltin> entries;
    bool registered = false;
};
Table& table() {
    static Table t;
    return t;
}

}  // namespace

bool PdBuiltins::add(const char* name, void (*setup)()) noexcept {
    if (name == nullptr || *name == '\0' || setup == nullptr) return false;
    Table& t = table();
    const std::lock_guard<std::mutex> lock(t.mutex);
    // After registerAll, a new entry would reach only whichever instance
    // happens to be current -- see the header. Refused, so that the failure is
    // "the external is missing" at a point someone is looking, rather than
    // "the external works in one device and not the next".
    if (t.registered) return false;
    for (const auto& e : t.entries) {
        if (std::strcmp(e.name, name) == 0) return false;
    }
    t.entries.push_back(PdBuiltin{name, setup});
    return true;
}

std::size_t PdBuiltins::count() noexcept {
    Table& t = table();
    const std::lock_guard<std::mutex> lock(t.mutex);
    return t.entries.size();
}

const char* PdBuiltins::nameAt(std::size_t index) noexcept {
    Table& t = table();
    const std::lock_guard<std::mutex> lock(t.mutex);
    return index < t.entries.size() ? t.entries[index].name : nullptr;
}

bool PdBuiltins::registered() noexcept {
    Table& t = table();
    const std::lock_guard<std::mutex> lock(t.mutex);
    return t.registered;
}

std::size_t PdBuiltins::registerAll() noexcept {
    Table& t = table();
    // The setup functions are copied out and called with the lock RELEASED. A
    // setup function calls into Pd, and Pd is entitled to call back into
    // anything; holding this mutex across that is a deadlock waiting for the
    // first external that registers a second class.
    std::vector<PdBuiltin> todo;
    {
        const std::lock_guard<std::mutex> lock(t.mutex);
        if (t.registered) return 0;
        t.registered = true;
        todo = t.entries;
    }
    for (const auto& e : todo) e.setup();
    return todo.size();
}

}  // namespace adi::device
