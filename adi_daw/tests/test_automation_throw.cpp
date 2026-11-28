// SPDX-License-Identifier: AGPL-3.0-or-later
// AU-10 (ADR-0254 d3): the Throw. A rise, a hold and a fall written over a time
// selection through AU-02's shared splice, in one undo step, with the lane
// outside the selection and its release unchanged.
#include "adi/automation_throw.hpp"
#include "adi/blob.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace adi;
using namespace adi::automation;
int checks = 0, failures = 0;
void check(bool ok, const std::string &message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", message.c_str());
    }
}
std::unique_ptr<Store> project(const std::filesystem::path &path, int base) {
    StoreError error = StoreError::Ok;
    auto store = Store::createEmpty(path, error);
    if (!store) throw std::runtime_error("create throw project");
    // A send's lane on a return's routing row is an ordinary global lane; the
    // Throw sees only the lane, so the fixture names a track parameter.
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Throw');"
                     "INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Vocal');"
                     "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,default_value,min_value,max_value) "
                     "VALUES(2,'track',1,'volume',0.25,0.0,1.0)");
    store->db().exec("UPDATE automation_lanes SET time_base=" + std::to_string(base));
    if (base == 0)
        store->db().exec("INSERT INTO tempo_map(id,pos_ticks,bpm,curve) VALUES(20,1000000,90,0),(21,2000000,150,0)");
    return store;
}
void stream(Store &store, const std::vector<AutomationPoint> &points, int base) {
    const auto bytes = writeStream<AutomationPoint>(FourCC::Automation, points, 1,
                                                    StreamFlags::SortedByTime | (base ? StreamFlags::TimeIsNanos : 0));
    SQLite::Statement insert(store.db(), "INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(4,2,NULL,?)");
    insert.bind(1, bytes.data(), static_cast<int>(bytes.size()));
    insert.exec();
}
AutomationPoint at(std::int64_t time, double value) {
    AutomationPoint p{};
    p.time = time;
    p.value = value;
    p.curve = 1;
    return p;
}
std::string lanes(Store &store) {
    std::string text;
    SQLite::Statement rows(store.db(), "SELECT id,lane_id,hex(data) FROM automation_data ORDER BY id");
    while (rows.executeStep())
        text += rows.getColumn(0).getString() + ":" + rows.getColumn(1).getString() + ":" +
                rows.getColumn(2).getString() + ";";
    return text;
}
double value(Store &store, std::int64_t native) {
    std::string error;
    const auto v = laneValueAt(store, 2, native, 48000, error);
    if (!v) throw std::runtime_error("lane value: " + error);
    return *v;
}

void throwOn(const std::filesystem::path &directory, int base, bool existing, std::int64_t riseUnits) {
    const std::string name = "throw-" + std::to_string(base) + (existing ? "-curve" : "-empty") +
                             "-rise" + std::to_string(riseUnits);
    auto store = project(directory / (name + ".adi"), base);
    const std::int64_t unit = base ? 1000000 : 10000;
    if (existing) stream(*store, {at(0, 0.0), at(150 * unit, 0.6), at(400 * unit, 0.2)}, base);
    const auto before = lanes(*store);
    std::vector<double> old;
    for (std::int64_t t = 0; t <= 400; ++t) old.push_back(value(*store, t * unit));

    ThrowSpec spec;
    spec.begin = 100 * unit;
    spec.end = 200 * unit;
    spec.rise = riseUnits * unit;
    spec.release = 50 * unit;
    spec.open = 0.9;
    const auto prepared = prepareThrow(*store, 2, spec, 48000);
    if (!prepared.ok) std::printf("prepare diagnostic %s\n", prepared.error.c_str());
    check(prepared.ok && prepared.requests.size() == 1, name + ": the Throw is one sealed splice");
    check(lanes(*store) == before, name + ": preparing writes nothing");
    const auto committed = OpJournal(*store).commit(prepared.requests);
    check(committed.ok, name + ": the Throw commits");
    if (!committed.ok) return;

    // Outside [begin, end + release] the lane is what it was; inside, the
    // shape; at both edges the lane's own value, so the old curve returns.
    double outside = 0;
    bool shape = true;
    for (std::int64_t t = 0; t <= 400; ++t) {
        const double now = value(*store, t * unit), was = old[static_cast<std::size_t>(t)];
        if (t < 100 || t > 250) outside = std::max(outside, std::abs(now - was));
        else if (t >= 100 + riseUnits && t <= 200) shape = shape && std::abs(now - 0.9) < 1e-9;
        else if (t > 200 && t < 250) {
            const double closed = old[250], x = static_cast<double>(t - 200) / 50.0;
            shape = shape && std::abs(now - (0.9 + (closed - 0.9) * x)) < 1e-6;
        }
    }
    check(outside < 1e-9, name + ": the lane outside the selection and its release is unchanged");
    check(shape, name + ": it holds open over the selection and falls over the release");
    check(std::abs(value(*store, 250 * unit) - old[250]) < 1e-9, name + ": the old curve returns at the release's edge");
    if (riseUnits > 0)
        check(std::abs(value(*store, 100 * unit) - old[100]) < 1e-9, name + ": it rises from the lane's own value");
    else
        check(std::abs(value(*store, 100 * unit) - 0.9) < 1e-9, name + ": with no rise it opens at once");
    History history(*store);
    check(history.undo().ok && lanes(*store) == before, name + ": one Undo removes every point");
    check(history.redo().ok && std::abs(value(*store, 150 * unit) - 0.9) < 1e-9, name + ": Redo throws again");
}

void refusals(const std::filesystem::path &directory) {
    auto store = project(directory / "refusals.adi", 1);
    const std::int64_t unit = 1000000;
    const auto refused = [&](ThrowSpec spec, std::int64_t lane, const char *what) {
        const auto prepared = prepareThrow(*store, lane, spec, 48000);
        check(!prepared.ok && prepared.requests.empty() && !prepared.error.empty(), what);
    };
    ThrowSpec good;
    good.begin = 100 * unit;
    good.end = 200 * unit;
    good.release = 10 * unit;
    auto spec = good;
    spec.end = spec.begin;
    refused(spec, 2, "a Throw needs a time selection");
    spec = good;
    spec.release = 0;
    refused(spec, 2, "a Throw needs a release");
    spec = good;
    spec.rise = 101 * unit;
    refused(spec, 2, "a rise longer than the selection is refused");
    spec = good;
    spec.open = std::nan("");
    refused(spec, 2, "a Throw needs a finite open value");
    refused(good, 99, "a Throw needs its lane");
    check(!History(*store).canUndo(), "a refused Throw leaves no undo step");
}
} // namespace

int main() {
    try {
        test::TempDirectory temp("automation_throw", "projects");
        for (const int base : {0, 1})
            for (const bool existing : {false, true})
                for (const std::int64_t rise : {std::int64_t{0}, std::int64_t{10}})
                    throwOn(temp.path(), base, existing, rise);
        refusals(temp.path());
    } catch (const std::exception &e) {
        check(false, std::string("the suite threw: ") + e.what());
    }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
