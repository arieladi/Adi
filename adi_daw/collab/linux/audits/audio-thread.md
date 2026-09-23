# Audio-thread audit — 2026-09-23

Direct read-only audit by linux (Codex), reviewed main `9e8d561` plus #70's
separate tested parameter-capture layer. Gemini CLI refused service for its
installed client (`IneligibleTierError`: client no longer supported), so no
findings are attributed to it. No credentials or repository tools were given
to Gemini. The unchanged engine/host files below were inspected directly.

## Verified finding for win: main-thread latency callback on audio path (high)

`Graph::process` calls `forwardEvents`; for a through-node with queued events,
`src/adi/engine/graph.cpp:850–852` reads its node latency. `DeviceNode` forwards
that call (`src/juce/device_model.cpp:182–192`) to `ClapDevice::latencySamples`
(`src/juce/clap_host.cpp:555–578`), which invokes the plugin extension getter.
`third_party/clap/include/clap/ext/latency.h:12–14` permits that getter only on
the **main thread**, while activating or active.

Mode 6 of the [standalone contract probe](clap-host-contract.md#reproduction)
constructs a CLAP effect feeding a sum, prepares on main, queues a note on the
effect, and renders on a worker. The fake getter records its thread and
contains a deliberate `operator new`/`delete` pair, an operation permitted in
a main-thread getter. Allocation counting encloses only `graph.process`, not
thread construction, prepare or destruction. Both GCC and Clang print:

```
audio_latency_queries=1 callback_allocations=1
FAIL contract probe 6
```

Thus the **host reaches a main-thread-only callback from audio**, and that
reachability can introduce allocation. This is not a claim that every real
CLAP plugin allocates in its latency getter. It is a deterministic example
of a permitted getter operation violating our process-path requirement.
The original allocation fixtures use synthetic nodes/devices and cannot
establish this CLAP getter boundary. The planted allocating getter makes the
new assertion fail without changing a production file. Proposed owner action:
cache latency off-callback and use that audio-safe value for event forwarding;
win decides the design. No fix applied here.

## Process-path coverage

| Path inspected | Allocation/lock/blocking review and limits |
|---|---|
| `Graph::process`, `forwardEvents`, `computeSplits`, `runNode`, `accumulate`, `DelayLine` in `src/adi/engine/graph.cpp` | Buffers, split marks, event and delay storage are prepared off-callback. Event overflow is counted; insertion sort uses fixed storage. Delay handover copies existing rings. No direct allocation, mutex, string building or `std::function` call identified in these processing methods. C6 is the indirect plugin-getter exception. Sorting cost is bounded by prepared capacity, not a wall-time guarantee. |
| `GraphHost::process`, `handover`, `fadeIn` in `src/adi/engine/host.cpp:51–143` | Walks prepared edge lists, copies existing history, announces only after old graph access, then renders/fades. Rebuild's maps, vectors, strings and supplier callbacks are outside this path. Existing `adi_host_tests` allocation checks pass; arbitrary plugin processing remains a trust boundary. |
| `SnapshotPublisher::AudioRead`, `peek`, `announce`, `collect` in `src/adi/engine/publisher.hpp` | Reader operations are atomic loads/stores. Retired-list mutex, vector growth and deletion occur on publisher/collector side. The strict reclamation predicate was independently mutation-tested below. Single forward-moving reader, valid ownership and stopped audio at destruction are required, not proven by the type system. |
| `MixNode::process`, `SumNode::process`, `GainNode::process` | Buffer copies/clears, scalar arithmetic and existing event iteration. No allocating or blocking operation identified; user virtual-node implementations are not universally certified. |
| `DeviceNode::process` (`src/juce/device_model.cpp:153–156`) | Bypass copies or invokes the device. No direct allocation. Its latency forwarding outside this small method is C6. Concurrent mutation of bypass/always flags is the already-recorded mac ownership question; no new race finding is asserted without a concurrency reproduction. |
| `ClapDevice::process` (`src/juce/clap_host.cpp:764–908`) | Prepared audio buses, bounded input event storage and fixed routed-event storage; insertion sort rather than allocating stable_sort. External plugin process is an untrusted realtime boundary. Public unprepared event-list use is outside the inspected prepared process contract. C2/C3 in the CLAP report concern lifecycle/frame validity. |
| `Vst3Device::process` (`src/juce/vst3_host.cpp:392–528`) and `vst3_events.*` | Static review of prepared scratch/event/parameter queues, raw processor path, and JUCE fallback. No new proven allocation defect. **Runtime coverage unavailable** here: JUCE/VST3 host execution remains mac/win's. A source-level read is not an allocation or lock guarantee for JUCE or a plugin. |
| `Session::process` (`src/adi/engine/session.cpp:403`) | Direct graph-host call. Loader, state strings, mirror operations and suppliers are on loading/rebuild paths, not called here. |
| `DeviceCore::process` (`src/juce/device_core.cpp:103–143`) | Validates granted size, fills stack AudioIo, invokes processor, updates counters. No direct allocation, locks or waiting. Device lifecycle changes must obey the caller's stopped-audio protocol. |
| `ParamEditCapture::push` (#70) | Fixed vector slot and lock-free atomic loads/stores; no map access, retries, clocks or callbacks. The required allocating-push plant fails case k; GCC TSan passes the 10,000-gesture test. Consumer map/output allocations are intentionally off-callback. |

No direct `std::function` invocation or string construction was found on the
listed core callback paths. Plugin virtual calls can still throw or block;
`noexcept` prevents propagation, not those behaviors. No claim is made that
all third-party implementations obey their contracts.

## Publisher memory orders and planted guard

Code matches the stated direction: `publish` release-exchanges the fully
initialized pointer; `AudioRead`/`peek` acquire-load it. `announce` or the
AudioRead constructor release-stores the sequence; `collect` acquire-loads
it and only frees strictly older snapshots. GraphHost performs old-state
handover **before** announcing the new snapshot. None of this permits two
concurrent readers or concurrent mutation of a published graph.

A standalone test below passes against the original header. In a scratch
copy only, replacing `if (inUse > (*it)->seq)` with
`if (inUse >= (*it)->seq)` yields exit 1:

```
FAIL: announced snapshot freed while reader still holds it
```

The test checks destructor counts before dereferencing a freed pointer, so
it detects the defect without needing a use-after-free crash. The production
header was never edited. This checks the strict boundary; it is not a proof
of every weak-memory execution. Existing whole-tree GCC TSan coverage and
publisher stress tests are additional evidence, not a formal proof.

Copy `src/adi/engine/publisher.hpp` beside this test as
`publisher-under-test.hpp`, compile with `g++ -std=c++20 -O2`, and repeat with
the single predicate change to reproduce the clean/plant pair.

```cpp
#include "publisher-under-test.hpp"
#include <cstdio>
using namespace adi::engine;
struct Item final : Sequenced { int& deaths; explicit Item(int& d):deaths(d){} ~Item() override{++deaths;} };
int main(){
 int deaths=0;SnapshotPublisher<Item> p;
 p.publish(std::make_unique<Item>(deaths));
 SnapshotPublisher<Item>::AudioRead first(p);
 p.publish(std::make_unique<Item>(deaths));
 const auto before=p.collect();
 if(before!=0 || deaths!=0){std::puts("FAIL: announced snapshot freed while reader still holds it");return 1;}
 SnapshotPublisher<Item>::AudioRead second(p);
 const auto after=p.collect();
 if(after!=1 || deaths!=1){std::puts("FAIL: older snapshot not reclaimed after reader advanced");return 1;}
 std::puts("PASS: strict reclamation guard");
}

```
