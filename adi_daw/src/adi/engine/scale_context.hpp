// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/clip_details.hpp"
#include <memory>
#include <span>
#include <string_view>
namespace adi::engine {
struct TuningDegree {
    std::int64_t id=0;
    double cents=0;
    std::string name;
};
struct TuningDefinition {
    std::int64_t id=0;
    std::string name,source,description;
    double periodCents=1200;
    std::vector<TuningDegree> degrees;
};
struct ScaleDegree : TuningDegree {bool inScale=false;};
struct ScalePitch {
    std::int64_t degree=0,period=0;
    std::size_t index=0;
    double cents=0;
    bool inScale=false;
    double distanceCents=0;
};
struct MidiPitch {
    std::uint8_t key=0;
    float tuningCents=0;
    double errorCents=0; // difference from the offered double pitch
};
// Native ANOT representation, with explicit error bound. No silent arbitrary
// double-to-float promise; the actual storage error is returned to the owner.
std::optional<MidiPitch> encodeMidiPitch(double cents,double maxErrorCents=0.0001) noexcept;
struct ScaleData;
struct ScaleGrid;
class ScaleContext {
public:
    [[nodiscard]] const ClipScale* scale() const noexcept;
    [[nodiscard]] double periodCents() const noexcept;
    [[nodiscard]] std::string_view tuningName() const noexcept;
    [[nodiscard]] std::string_view tuningSource() const noexcept;
    [[nodiscard]] std::string_view tuningDescription() const noexcept;
    [[nodiscard]] std::span<const ScaleDegree> degrees() const noexcept;
    [[nodiscard]] bool includesDegree(std::int64_t id) const noexcept;
    // Pure bounded queries: caller retains this immutable context for its graph
    // generation. No Store, allocation, locks or implicit retuning of notes.
    // tonicCents is an explicit absolute pitch of degree zero. It is never
    // inferred from a non-octave period or reconstructed from a MIDI channel.
    [[nodiscard]] std::optional<ScalePitch> nearest(double pitchCents,double tonicCents,bool scaleOnly=false) const noexcept;
    [[nodiscard]] std::optional<ScalePitch> match(double pitchCents,double tonicCents,double toleranceCents=0.0001) const noexcept;
    [[nodiscard]] bool contains(double pitchCents,double tonicCents,double toleranceCents=0.0001) const noexcept;
    // Owner-only ruler/fold projection. Names are authored degree names, or
    // numeric degree IDs when unnamed. Native key+cent offset is optional.
    [[nodiscard]] ScaleGrid grid(double lowCents,double highCents,double tonicCents,bool scaleOnly=false,std::size_t maxRows=8192) const;
private:
    explicit ScaleContext(std::shared_ptr<const ScaleData> data):data_(std::move(data)){}
    std::shared_ptr<const ScaleData> data_;
    friend std::optional<ScaleContext> compileScaleContext(const ClipScale&,const std::optional<TuningDefinition>&,std::string&);
};
struct ScaleRow {ScalePitch pitch;std::string name;std::optional<MidiPitch> midi;};
struct ScaleGrid {std::vector<ScaleRow> rows;std::string error;};
struct ScaleContextRead {std::optional<ScaleContext> context;std::string error;};
// Owner compilation accepts arbitrary valid scales over explicit tuning rows;
// it does not guess a Live preset list. Tuned keys use degree membership and
// ignore the legacy 12-bit mask (ADR-0178). Empty membership remains empty.
std::optional<ScaleContext> compileScaleContext(const ClipScale&,const std::optional<TuningDefinition>&,std::string& error);
// One coherent owner read, reusing N32 clip-local/inherited scale rules without
// decoding its note stream. No project writes or schema/new opcode.
ScaleContextRead readClipScaleContext(const Store&,std::int64_t clip);
} // namespace adi::engine
