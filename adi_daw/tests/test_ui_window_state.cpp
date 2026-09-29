// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/store.hpp"
#include "adi/ui/window_state.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <thread>
using namespace adi;
using namespace adi::ui;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}
} // namespace
int main() {
    test::TempDirectory temp("ui_state", "roundtrip");
    StoreError err{};
    auto store = Store::create(temp.path() / "p.adi", err);
    if (!store)
        return 2;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'UI test')");
    ViewStateStore views(*store);
    std::string error;
    WindowState state;
    check(!state.panels.setWidth(Panel::Browser, 333, 1400), "resize browser");
    check(!state.panels.swapSides(1400), "swap panel identities");
    state.zoom = 2;
    state.deviceHeight = 245;
    state.docked = false;
    check(views.save("main", state, error), "save view");
    const auto restored = views.load("main");
    check(restored.panels.slots()[0].panel == Panel::Mixer, "restore order");
    check(restored.panels.find(Panel::Browser)->width == 333, "width follows panel");
    check(restored.zoom == 2 && restored.deviceHeight == 245 && !restored.docked,
          "restore zoom and dock");
    check(views.load("other").zoom == 1, "windows have independent view state");
    check(store->db().execAndGet("SELECT count(*) FROM ops").getInt() == 0,
          "view persistence creates no edit/undo entry");
    state.zoom = 0;
    check(!views.save("main", state, error), "invalid view refused");
    check(views.load("main").zoom == 2, "refusal preserves saved state");
    store->db().exec("UPDATE ui_view SET value='{broken'");
    check(views.load("main").zoom == 1, "malformed view defaults");
    store->db().exec("UPDATE ui_view SET value='{\"version\":99}'");
    check(views.load("main").zoom == 1, "future view defaults without crashing");
    auto readonly = Store::open(temp.path() / "p.adi", err, true);
    ViewStateStore ro(*readonly);
    check(!ro.save("main", WindowState{}, error), "read-only view refuses save");
    DirtySet a, b;
    check(a.drain() == DirtySet::All && b.drain() == DirtySet::All, "new windows initially dirty");
    a.mark(DirtySet::Transport);
    a.mark(DirtySet::Transport);
    check(a.drain() == DirtySet::Transport && a.drain() == 0 && b.drain() == 0,
          "coalescing and per-window isolation");
    TransportMailbox mailbox;
    engine::Transport transport;
    check(mailbox.post(TransportMailbox::Command::Play), "queue play");
    check(!transport.playing() && mailbox.desiredPlaying(),
          "UI never mutates driver; queued intent visible");
    mailbox.drain(transport);
    check(transport.playing() && mailbox.playing(), "driver applies command and feeds back");
    transport.advance(64);
    mailbox.publish(transport);
    check(mailbox.position() == 64, "driver position feedback");
    for (int i = 0; i < 16; ++i)
        check(mailbox.post(TransportMailbox::Command::Stop), "bounded queue accepts capacity");
    check(!mailbox.post(TransportMailbox::Command::Play), "full queue refuses without overwrite");
    mailbox.drain(transport);
    check(!transport.playing(), "queued stop survives saturation");
    // Two actual threads exercise release/acquire ownership, with a final stop sentinel.
    std::atomic<bool> finished{false};
    std::thread driver([&] {
        while (!finished.load(std::memory_order_acquire))
            mailbox.drain(transport);
        mailbox.drain(transport);
    });
    for (int i = 0; i < 10000; ++i) {
        while (!mailbox.post(i % 2 ? TransportMailbox::Command::Stop
                                   : TransportMailbox::Command::Play))
            std::this_thread::yield();
    }
    finished.store(true, std::memory_order_release);
    driver.join();
    check(!transport.playing() && !mailbox.playing(),
          "concurrent driver preserves ordered final stop");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
