/*
** Surge Synthesizer is Free and Open Source Software
**
** Surge is made available under the Gnu General Public License, v3.0
** https://www.gnu.org/licenses/gpl-3.0.en.html
**
** Copyright 2004-2021 by various individuals as described by the Git transaction log
**
** All source at: https://github.com/surge-synthesizer/surge.git
**
** Surge was a commercial product from 2004-2018, with Copyright and ownership
** in that period held by Claes Johanson at Vember Audio. Claes made Surge
** open source in September 2018.
*/
// Scalar adaptation of BYOD's SurgeWaveshapers::OJD, commit
// 1cf22b6ac802b9dc33cfc9f8dd6af5b3c3e40bc9. SIMD masks become branches;
// thresholds, knees and float arithmetic retain the upstream transfer.
#pragma once
namespace adi::dsp::byod {
inline float ojd(float input, float drive) noexcept {
    const float x = input * drive;
    if (x <= -1.7f)
        return -1.f;
    if (x >= 1.1f)
        return 1.f;
    if (x < -.3f) {
        const float z = x + .3f;
        return z + (1.f / (4 * (1 - .3f))) * (z * z) - .3f;
    }
    if (x > .9f) {
        const float z = x - .9f;
        return z - (1.f / (4 * (1 - .9f))) * (z * z) + .9f;
    }
    return x;
}
} // namespace adi::dsp::byod
