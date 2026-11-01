// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/store.hpp"
#include "adi/ui/session_state.hpp"
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
    state.follow = true;
    state.overview = false;
    state.inOut = false;
    state.largeTracks = false;
    state.gridSnap = false;
    state.gridFixed = true;
    state.gridTriplet = true;
    state.gridNarrow = 2;
    state.gridFixedQuarters = .125;
    check(views.save("main", state, error), "save view");
    const auto restored = views.load("main");
    check(restored.follow && !views.load("other").follow, "Follow is kept per window, off until switched on");
    check(!restored.overview && !restored.inOut && views.load("other").overview && views.load("other").inOut,
          "the Overview and In/Out are kept per window, shown until hidden");
    check(!restored.largeTracks && views.load("other").largeTracks,
          "Use Large Track Height is kept per window, on until switched off");
    check(!restored.gridSnap && restored.gridFixed && restored.gridTriplet && restored.gridNarrow == 2 &&
              restored.gridFixedQuarters == .125 && views.load("other").gridSnap && !views.load("other").gridFixed,
          "the editing grid is kept per window, snapping and adaptive until changed");
    {
        WindowState beyond = state;
        beyond.gridNarrow = 3;
        beyond.gridFixedQuarters = 1. / 32;
        check(views.save("beyond", beyond, error) && views.load("beyond").gridNarrow == 2 &&
                  views.load("beyond").gridFixedQuarters == .125,
              "a grid width beyond the lanes' menu loads as the nearest it names: Narrowest, 1/32");
    }
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
    {
        SessionState roSession(*readonly);
        Selection s;
        s.track = 3;
        check(roSession.select(s, error) && roSession.selection().track == 3,
              "a read-only project still selects, in memory");
    }
    // Selection is session state (ADR-0021): one per project, never an op.
    {
        SessionState session(*store);
        check(session.selection() == Selection{}, "a new project selects nothing");
        Selection s;
        s.track = 7;
        s.start = 10;
        s.end = 20;
        s.marked = true;
        s.insert = 10;
        const auto before = session.generation();
        check(session.select(s, error) && session.generation() == before + 1,
              "a new selection is a new generation");
        check(session.select(s, error) && session.generation() == before + 1,
              "the same selection again is no change");
        check(SessionState(*store).selection() == s, "it is kept in session_state and reloads");
        check(store->db().execAndGet("SELECT count(*) FROM ops").getInt() == 0,
              "and selecting writes no op");
        const auto blob = session.advisory();
        Selection other;
        other.track = 9;
        check(session.select(other, error), "select something else");
        session.restore(blob);
        check(session.selection() == s, "an undo's sel_before puts the selection back");
        session.restore(Payload::object());
        check(session.selection() == s, "a step without one leaves it alone");
        store->db().exec("UPDATE session_state SET value='{broken' WHERE key='selection'");
        check(SessionState(*store).selection() == Selection{},
              "a malformed row is a lost selection, never a failed open");
    }
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
