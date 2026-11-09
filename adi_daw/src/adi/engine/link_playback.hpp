// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/link_session.hpp"
#include "adi/engine/process.hpp"
#include "adi/engine/session.hpp"
#include <atomic>
#include <cstdint>
#include <optional>

namespace adi { class Store; }
namespace adi::engine {

// The driver returns value packets to the serialized document owner. There is
// no Store, graph preparation, allocation or cross-thread Transport access in
// process(). The caller transports copies through its normal driver mailbox.
class LinkPlayback final {
public:
    struct Preparation {
        std::uint64_t serial=0, generation=0;
        std::int64_t capturedMicros=0, boundaryTick=0;
        std::optional<double> tempo;
        bool reset=false;
        [[nodiscard]] explicit operator bool()const noexcept{return serial!=0;}
    };
    struct Result {
        LinkFrame frame;
        Preparation preparation;
        bool valid=false,waiting=false,started=false,stopped=false;
        std::int32_t startOffset=-1,stopOffset=-1;
        double phaseErrorFrames=0;
    };
    LinkPlayback(Session& session,LinkSession& link)noexcept;
    // Driver only, between calls. The position used by the next quantized start.
    [[nodiscard]] bool setLaunchTick(std::int64_t tick)noexcept;
    // buffer.outputLatencyMicros is PHYSICAL driver latency only; this adapter
    // adds the actually pinned graph's compensation exactly once.
    [[nodiscard]] Result process(const AudioIo&,const LinkBuffer&,const LinkCommand& = {})noexcept;
    // Deterministic native-consumer seam. Frame timestamps already include both
    // latencies and come from one SDK capture. This still runs the real Session.
    [[nodiscard]] Result processCaptured(const AudioIo&,const LinkFrame&,double quantum=4,
                                        const LinkCommand& = {})noexcept;
    // Serialized document owner; nowMicros uses the Link clock domain. Rejects
    // replaced, already consumed and older-than-two-seconds preparation packets.
    [[nodiscard]] bool prepare(const Store&,const Preparation&,std::int64_t nowMicros);
private:
    [[nodiscard]] Result consume(Session::RenderBlock&,const AudioIo&,const LinkFrame&,
                                 double quantum,const LinkCommand&)noexcept;
    Session& session_;
    LinkSession& link_;
    std::atomic<std::uint64_t> requested_{0},prepared_{0};
    Preparation pending_;
    std::int64_t launchTick_=0,lastChange_=0,launchMicros_=0,stopMicros_=0;
    bool linked_=false,haveChange_=false,pendingStart_=false,pendingStop_=false,anchored_=false,pendingLocal_=false;
    bool haveEnd_=false;
    std::int64_t expectedSample_=0;
    std::optional<std::int64_t> repairTick_;
    std::uint64_t expectedRevision_=0,lastGeneration_=0;
    double beatOffset_=0,performedBeat_=0,endBeat_=0,pendingLinkTempo_=0;
};
} // namespace adi::engine
