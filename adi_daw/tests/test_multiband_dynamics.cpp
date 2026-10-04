// SPDX-License-Identifier: GPL-3.0-or-later
// Multiband Dynamics against Ableton Live 11.2.7. Every reference number below was
// measured from Live's own renders of the same signals
// (collab/mac/2026-10-03-multiband-dynamics-pd.md, collab/mac/live-probe/).
#include "adi/dsp/multiband_dynamics.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <vector>
namespace {
thread_local bool countAllocations = false;
thread_local std::size_t allocations = 0;
} // namespace
void *operator new(std::size_t n) {
    if (countAllocations)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
namespace {
using MD = adi::dsp::MultibandDynamics;
int checks = 0, failures = 0;
void check(bool ok, const char *text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", text);
    }
}
void near(double got, double want, double tol, const char *text) {
    check(std::abs(got - want) <= tol, text);
    if (std::abs(got - want) > tol)
        std::printf("     got %.6f want %.6f (tol %.4f)\n", got, want, tol);
}
// Live 11.2.7's single-band neutral response to a 0.5 impulse, first 64 samples.
const std::array<float, 64> kLiveImpulse{
    0x1.9791e6p-13f, 0x1.81108p-7f, 0x1.be1d54p-4f, 0x1.3bc95p-2f,
    0x1.a17cb4p-3f, -0x1.62f288p-3f, 0x1.df0c8p-9f, 0x1.9b5f12p-4f,
    -0x1.ebf88ep-4f, 0x1.6efec8p-4f, -0x1.5752eap-5f, -0x1.88311p-9f,
    0x1.297644p-5f, -0x1.cbde4p-5f, 0x1.062428p-4f, -0x1.00ec0ap-4f,
    0x1.c4d054p-5f, -0x1.6a41cep-5f, 0x1.029cb4p-5f, -0x1.336bfep-6f,
    0x1.bb62ep-8f, 0x1.f836f8p-9f, -0x1.a2c658p-7f, 0x1.46b7bcp-6f,
    -0x1.9fb3d8p-6f, 0x1.de6668p-6f, -0x1.02d19p-5f, 0x1.0c4aeep-5f,
    -0x1.0d3c78p-5f, 0x1.07330cp-5f, -0x1.f73a2ep-6f, 0x1.d78628p-6f,
    -0x1.b18964p-6f, 0x1.8731fp-6f, -0x1.5a2068p-6f, 0x1.2bae32p-6f,
    -0x1.f9e96cp-7f, 0x1.9da9ecp-7f, -0x1.43fe1ap-7f, 0x1.dbe4d4p-8f,
    -0x1.38990cp-8f, 0x1.3e5becp-9f, -0x1.056f4p-12f, -0x1.ce2158p-10f,
    0x1.d898ecp-9f, -0x1.5a04fep-8f, 0x1.bce93ep-8f, -0x1.0aa784p-7f,
    0x1.31d15p-7f, -0x1.54303p-7f, 0x1.7207bcp-7f, -0x1.8b9ec4p-7f,
    0x1.a13dap-7f, -0x1.b32cccp-7f, 0x1.c1b3e4p-7f, -0x1.cd18b8p-7f,
    0x1.d59ec8p-7f, -0x1.db86c4p-7f, 0x1.df0e42p-7f, -0x1.e06f88p-7f,
    0x1.dfe174p-7f, -0x1.dd9778p-7f, 0x1.d9c184p-7f, -0x1.d48c46p-7f,
};
// Live 11.2.7 at 44100 Hz (engine and export): the same response, 32 samples.
const std::array<float, 32> kLiveImpulse44k{
    0x1.9791e6p-13f, 0x1.81108p-7f, 0x1.be1d54p-4f, 0x1.3bc95p-2f,
    0x1.a17cb4p-3f, -0x1.62f288p-3f, 0x1.df0c8p-9f, 0x1.9b5f12p-4f,
    -0x1.ebf88ep-4f, 0x1.6efec8p-4f, -0x1.5752eap-5f, -0x1.88311p-9f,
    0x1.297644p-5f, -0x1.cbde4p-5f, 0x1.062428p-4f, -0x1.00ec0ap-4f,
    0x1.c4d054p-5f, -0x1.6a41cep-5f, 0x1.029cb4p-5f, -0x1.336bfep-6f,
    0x1.bb62ep-8f, 0x1.f836f8p-9f, -0x1.a2c658p-7f, 0x1.46b7bcp-6f,
    -0x1.9fb3d8p-6f, 0x1.de6668p-6f, -0x1.02d19p-5f, 0x1.0c4aeep-5f,
    -0x1.0d3c78p-5f, 0x1.07330cp-5f, -0x1.f73a2ep-6f, 0x1.d78628p-6f,
};
// Live 11.2.7 at 96000 Hz (engine and export): the same response, 32 samples.
const std::array<float, 32> kLiveImpulse96k{
    0x1.9791e6p-13f, 0x1.81108p-7f, 0x1.be1d54p-4f, 0x1.3bc95p-2f,
    0x1.a17cb4p-3f, -0x1.62f288p-3f, 0x1.df0c8p-9f, 0x1.9b5f12p-4f,
    -0x1.ebf88ep-4f, 0x1.6efec8p-4f, -0x1.5752eap-5f, -0x1.88311p-9f,
    0x1.297644p-5f, -0x1.cbde4p-5f, 0x1.062428p-4f, -0x1.00ec0ap-4f,
    0x1.c4d054p-5f, -0x1.6a41cep-5f, 0x1.029cb4p-5f, -0x1.336bfep-6f,
    0x1.bb62ep-8f, 0x1.f836f8p-9f, -0x1.a2c658p-7f, 0x1.46b7bcp-6f,
    -0x1.9fb3d8p-6f, 0x1.de6668p-6f, -0x1.02d19p-5f, 0x1.0c4aeep-5f,
    -0x1.0d3c78p-5f, 0x1.07330cp-5f, -0x1.f73a2ep-6f, 0x1.d78628p-6f,
};
// Band magnitudes (dB) of Live's solo impulse responses at kFreqs.
const std::array<double, 7> kFreqs{50, 120, 500, 1000, 2500, 5000, 10000};
const std::array<double, 7> k_n_soloLow{-0.272263, -6.038481, -49.636954, -73.676140, -105.484711, -130.379894, -151.100811};
const std::array<double, 7> k_n_soloMid{-30.746575, -6.034369, -0.043852, -0.220761, -6.023310, -24.831009, -49.389301};
const std::array<double, 7> k_n_soloHigh{-110.254159, -112.000271, -55.994760, -32.118547, -6.023221, -0.514196, -0.029831};
const std::array<double, 7> k_n_neutral{-0.015994, -0.015803, -0.001327, -0.000958, -0.002620, -0.001231, -0.000312};
const std::array<double, 7> k_x1000_8000_soloLow{-0.000317, -0.002122, -0.527485, -6.023348, -32.119376, -56.231647, -81.260439};
const std::array<double, 7> k_x1000_8000_soloMid{-104.004670, -73.658445, -24.620125, -6.027311, -0.297221, -1.187124, -11.072846};
const std::array<double, 7> k_x1000_8000_soloHigh{-128.411800, -127.717585, -97.110802, -73.056695, -41.217503, -17.995622, -2.851559};
// mbd_a/n_neutral.wav: Live's response to a 0.5 impulse, 64 samples
const std::array<float, 64> kLiveNeutral3Band{
    0x1.3fcb38p-13f, 0x1.11bffep-7f, 0x1.0d30e4p-4f, 0x1.efcb1ep-4f,
    -0x1.c0e2d8p-4f, -0x1.3ee328p-2f, 0x1.15ba08p-4f, 0x1.e871b8p-4f,
    -0x1.e587bcp-6f, 0x1.98e57ap-3f, 0x1.6a46ap-6f, 0x1.9f7e18p-4f,
    0x1.37277cp-4f, 0x1.069598p-8f, 0x1.5a9d6p-4f, -0x1.805fd2p-5f,
    0x1.fae7ap-5f, -0x1.e0d744p-5f, 0x1.d9467p-6f, -0x1.8dc304p-5f,
    -0x1.60b5ep-10f, -0x1.efc618p-6f, -0x1.852244p-6f, -0x1.8b819p-7f,
    -0x1.35f44cp-5f, 0x1.15c6a4p-9f, -0x1.70acb8p-5f, 0x1.6ecd0ep-7f,
    -0x1.81ae0cp-5f, 0x1.fd3dap-7f, -0x1.764ddep-5f, 0x1.092bccp-6f,
    -0x1.58e5bp-5f, 0x1.d3dfap-7f, -0x1.3100f4p-5f, 0x1.635a28p-7f,
    -0x1.03f8d8p-5f, 0x1.b5b948p-8f, -0x1.ab1344p-6f, 0x1.33482p-9f,
    -0x1.5084acp-6f, -0x1.d7011p-10f, -0x1.f74982p-7f, -0x1.6a979cp-8f,
    -0x1.5d0248p-7f, -0x1.1f0022p-7f, -0x1.a8a224p-8f, -0x1.76d798p-7f,
    -0x1.776e24p-9f, -0x1.bd187p-7f, 0x1.b096p-13f, -0x1.f2daccp-7f,
    0x1.6a6e0cp-9f, -0x1.0cd23p-6f, 0x1.3e5412p-8f, -0x1.19951ep-6f,
    0x1.ac31ap-8f, -0x1.20951ap-6f, 0x1.011ddp-7f, -0x1.22a9ap-6f,
    0x1.21e16ep-7f, -0x1.209c34p-6f, 0x1.39eeap-7f, -0x1.1b24fep-6f,
};
// mbd_b/x30_300_soloLow.wav: Live's response to a 0.5 impulse, 64 samples
const std::array<float, 64> kLiveLow30Hz{
    0x1.8b4edp-53f, 0x1.14fd9cp-45f, 0x1.003656p-40f, 0x1.750528p-37f,
    0x1.1e1fcap-34f, 0x1.1028c4p-32f, 0x1.6b9668p-31f, 0x1.7d06a6p-30f,
    0x1.55dc58p-29f, 0x1.14aa62p-28f, 0x1.9f98f2p-28f, 0x1.2737c2p-27f,
    0x1.919d44p-27f, 0x1.07dc24p-26f, 0x1.5107fap-26f, 0x1.a4700cp-26f,
    0x1.010fa6p-25f, 0x1.35005cp-25f, 0x1.6df77p-25f, 0x1.abd5ecp-25f,
    0x1.ee755cp-25f, 0x1.1ad33ep-24f, 0x1.4099ap-24f, 0x1.686ff8p-24f,
    0x1.92355ap-24f, 0x1.bdc744p-24f, 0x1.eb00c4p-24f, 0x1.0cde2p-23f,
    0x1.24e91cp-23f, 0x1.3d8db8p-23f, 0x1.56b7aep-23f, 0x1.7053p-23f,
    0x1.8a4b66p-23f, 0x1.a48d24p-23f, 0x1.bf04bp-23f, 0x1.d99f26p-23f,
    0x1.f449d4p-23f, 0x1.077974p-22f, 0x1.14c492p-22f, 0x1.21fe48p-22f,
    0x1.2f1ed2p-22f, 0x1.3c1f04p-22f, 0x1.48f844p-22f, 0x1.55a48p-22f,
    0x1.621dcep-22f, 0x1.6e5f84p-22f, 0x1.7a656p-22f, 0x1.862b8cp-22f,
    0x1.91aef8p-22f, 0x1.9ced26p-22f, 0x1.a7e478p-22f, 0x1.b293ecp-22f,
    0x1.bcfb38p-22f, 0x1.c71abep-22f, 0x1.d0f38ep-22f, 0x1.da874ap-22f,
    0x1.e3d8aap-22f, 0x1.ecebp-22f, 0x1.f5c1a8p-22f, 0x1.fe6128p-22f,
    0x1.036776p-21f, 0x1.07888p-21f, 0x1.0b96fap-21f, 0x1.0f9694p-21f,
};
// mbd_dsr96/sr96_neutral.wav: Live's response to a 0.5 impulse, 32 samples
const std::array<float, 32> kLiveNeutral96k{
    0x1.6905e2p-13f, 0x1.44d5a8p-7f, 0x1.5b8b6ap-4f, 0x1.96acecp-3f,
    -0x1.82928p-9f, -0x1.4d9f5ap-2f, -0x1.ac87ap-5f, 0x1.d72fc2p-6f,
    -0x1.2c8c44p-3f, 0x1.7d10bcp-4f, -0x1.66e0bcp-5f, 0x1.1deb84p-5f,
    0x1.ea499cp-5f, -0x1.129d64p-7f, 0x1.af66f2p-4f, -0x1.09d218p-6f,
    0x1.ab5b9cp-4f, -0x1.c24ap-9f, 0x1.47ab02p-4f, 0x1.e2ca58p-7f,
    0x1.91f21cp-5f, 0x1.e79a9p-6f, 0x1.50ae74p-6f, 0x1.3c5c18p-5f,
    -0x1.ea4998p-10f, 0x1.52ba6p-5f, -0x1.1e564ep-6f, 0x1.41daacp-5f,
    -0x1.b994c4p-6f, 0x1.170a54p-5f, -0x1.01fc7cp-5f, 0x1.bc3984p-6f,
};
// mbd_d/coef_rel5000_pk: DC -6 dB for 1 s then -80 dB; Live's output 10, 100, 1000 and 4000 ms after the drop
const std::array<float, 4> kLiveRelease5000{0x1.d96626p-20f, 0x1.9f80bap-20f, 0x1.dc1c44p-19f, 0x1.9bbc58p-15f};
// mbd_d/init_lead0.05_up_pk: 50 ms of silence then DC -60 dB; Live's output 1, 10, 100 and 500 ms after the DC starts
const std::array<float, 4> kLiveInitUpward{0x1.cbe1p-9f, 0x1.ce224p-9f, 0x1.0b23bp-8f, 0x1.ea11ecp-8f};

// Live's clip declick at 48 kHz, measured: the first 192 gains of every clip (not the device)
const std::array<float, 192> kLiveClipFade{
    0.0f, 0x1.2f684ep-9f, 0x1.2f684ep-8f, 0x1.c71c74p-8f,
    0x1.2f684ep-7f, 0x1.7b4262p-7f, 0x1.c71c74p-7f, 0x1.097b44p-6f,
    0x1.2f684ep-6f, 0x1.555558p-6f, 0x1.7b4262p-6f, 0x1.a12f6cp-6f,
    0x1.c71c74p-6f, 0x1.ed097ep-6f, 0x1.097b44p-5f, 0x1.1c71cap-5f,
    0x1.2f684ep-5f, 0x1.425ed2p-5f, 0x1.555558p-5f, 0x1.684bdcp-5f,
    0x1.7b4262p-5f, 0x1.8e38e6p-5f, 0x1.a12f6cp-5f, 0x1.b425fp-5f,
    0x1.c71c74p-5f, 0x1.da12fap-5f, 0x1.ed097ep-5f, 0x1.000002p-4f,
    0x1.097b44p-4f, 0x1.12f686p-4f, 0x1.1c71cap-4f, 0x1.25ed0cp-4f,
    0x1.2f684ep-4f, 0x1.471c74p-4f, 0x1.5ed09ap-4f, 0x1.7684cp-4f,
    0x1.8e38e6p-4f, 0x1.a5ed0cp-4f, 0x1.bda132p-4f, 0x1.d55558p-4f,
    0x1.ed097ep-4f, 0x1.025ed2p-3f, 0x1.0e38e6p-3f, 0x1.1a12f8p-3f,
    0x1.25ed0cp-3f, 0x1.31c71ep-3f, 0x1.3da132p-3f, 0x1.497b44p-3f,
    0x1.555558p-3f, 0x1.612f6ap-3f, 0x1.6d097ep-3f, 0x1.78e39p-3f,
    0x1.84bda4p-3f, 0x1.9097b6p-3f, 0x1.9c71cap-3f, 0x1.a84bdcp-3f,
    0x1.b425fp-3f, 0x1.c00002p-3f, 0x1.cbda16p-3f, 0x1.d7b428p-3f,
    0x1.e38e3cp-3f, 0x1.ef684ep-3f, 0x1.fb4262p-3f, 0x1.038e3ap-2f,
    0x1.097b44p-2f, 0x1.112f6ap-2f, 0x1.18e39p-2f, 0x1.2097b6p-2f,
    0x1.284bdcp-2f, 0x1.300002p-2f, 0x1.37b428p-2f, 0x1.3f684ep-2f,
    0x1.471c74p-2f, 0x1.4ed098p-2f, 0x1.5684bep-2f, 0x1.5e38e4p-2f,
    0x1.65ed0ap-2f, 0x1.6da13p-2f, 0x1.755556p-2f, 0x1.7d097cp-2f,
    0x1.84bda2p-2f, 0x1.8c71c8p-2f, 0x1.9425eep-2f, 0x1.9bda14p-2f,
    0x1.a38e3ap-2f, 0x1.ab426p-2f, 0x1.b2f686p-2f, 0x1.baaaacp-2f,
    0x1.c25edp-2f, 0x1.ca12f6p-2f, 0x1.d1c71cp-2f, 0x1.d97b42p-2f,
    0x1.e12f68p-2f, 0x1.e8e38ep-2f, 0x1.f097b4p-2f, 0x1.f84bdap-2f,
    0x1.p-1f, 0x1.03da14p-1f, 0x1.07b426p-1f, 0x1.0b8e38p-1f,
    0x1.0f684cp-1f, 0x1.13426p-1f, 0x1.171c72p-1f, 0x1.1af684p-1f,
    0x1.1ed098p-1f, 0x1.22aaacp-1f, 0x1.2684bep-1f, 0x1.2a5edp-1f,
    0x1.2e38e4p-1f, 0x1.3212f8p-1f, 0x1.35ed0ap-1f, 0x1.39c71cp-1f,
    0x1.3da13p-1f, 0x1.417b44p-1f, 0x1.455556p-1f, 0x1.492f68p-1f,
    0x1.4d097cp-1f, 0x1.50e39p-1f, 0x1.54bda2p-1f, 0x1.5897b4p-1f,
    0x1.5c71c8p-1f, 0x1.604bdcp-1f, 0x1.6425eep-1f, 0x1.68p-1f,
    0x1.6bda14p-1f, 0x1.6fb428p-1f, 0x1.738e3ap-1f, 0x1.77684cp-1f,
    0x1.7b426p-1f, 0x1.7e38e4p-1f, 0x1.812f6ap-1f, 0x1.8425eep-1f,
    0x1.871c72p-1f, 0x1.8a12f8p-1f, 0x1.8d097cp-1f, 0x1.9p-1f,
    0x1.92f686p-1f, 0x1.95ed0ap-1f, 0x1.98e38ep-1f, 0x1.9bda14p-1f,
    0x1.9ed098p-1f, 0x1.a1c71cp-1f, 0x1.a4bda2p-1f, 0x1.a7b426p-1f,
    0x1.aaaaacp-1f, 0x1.ada13p-1f, 0x1.b097b4p-1f, 0x1.b38e3ap-1f,
    0x1.b684bep-1f, 0x1.b97b42p-1f, 0x1.bc71c8p-1f, 0x1.bf684cp-1f,
    0x1.c25edp-1f, 0x1.c55556p-1f, 0x1.c84bdap-1f, 0x1.cb425ep-1f,
    0x1.ce38e4p-1f, 0x1.d12f68p-1f, 0x1.d425ecp-1f, 0x1.d71c72p-1f,
    0x1.da12f6p-1f, 0x1.db425ep-1f, 0x1.dc71c6p-1f, 0x1.dda12ep-1f,
    0x1.ded098p-1f, 0x1.ep-1f, 0x1.e12f68p-1f, 0x1.e25edp-1f,
    0x1.e38e38p-1f, 0x1.e4bdap-1f, 0x1.e5ed0ap-1f, 0x1.e71c72p-1f,
    0x1.e84bdap-1f, 0x1.e97b42p-1f, 0x1.eaaaaap-1f, 0x1.ebda12p-1f,
    0x1.ed097cp-1f, 0x1.ee38e4p-1f, 0x1.ef684cp-1f, 0x1.f097b4p-1f,
    0x1.f1c71cp-1f, 0x1.f2f684p-1f, 0x1.f425ecp-1f, 0x1.f55556p-1f,
    0x1.f684bep-1f, 0x1.f7b426p-1f, 0x1.f8e38ep-1f, 0x1.fa12f6p-1f,
    0x1.fb425ep-1f, 0x1.fc71c8p-1f, 0x1.fda13p-1f, 0x1.fed098p-1f,
};

// mbd_a/st_above-20_r-0.75_peak_knee.wav: Live's output mid-stair at -28, -24, -20, -16 dB
const std::array<float, 4> kLiveKneeStairs{0x1.43534p-5f, 0x1.de416cp-5f, 0x1.49ffe8p-4f, 0x1.a92acp-4f};
// mbd_a/st_above-20_r-0.75_rms.wav: Live's output mid-stair at -16, -8, 0, 6 dB
const std::array<float, 4> kLiveRmsStairs{0x1.cb82ccp-4f, 0x1.2124e4p-3f, 0x1.6c190cp-3f, 0x1.b0cd28p-3f};

#if defined(__APPLE__)
// Live's reference renders were made on macOS. The crossovers, gain knobs and frequency
// mapping go through the platform's float sinf, cosf, powf, log10f and log2f, as Live's
// own do, so there they agree to the bit. A libm that rounds one of those an ulp the
// other way moves a low-frequency biquad (Live on Windows differs the same way), so
// elsewhere the same checks run against a tolerance.
constexpr bool kLiveLibm = true;
#else
constexpr bool kLiveLibm = false;
#endif
template <std::size_t N>
bool sameAsLive(const std::vector<float> &got, const std::array<float, N> &want, double elsewhere) {
    for (std::size_t i = 0; i < N; ++i) {
        const double d = std::abs(static_cast<double>(got[i]) - want[i]);
        if (kLiveLibm ? got[i] != want[i] : d > elsewhere) {
            std::printf("     sample %zu: got %a want %a\n", i, static_cast<double>(got[i]),
                        static_cast<double>(want[i]));
            return false;
        }
    }
    return true;
}

// The probes' baseline: every band active and neutral, RMS, hard knee, 10/100 ms.
void neutral(MD &m) {
    m.set(MD::SoftKnee, 0);
    for (int b = 0; b < 3; ++b) {
        m.set(static_cast<MD::Param>(MD::AboveThresholdLow + b), 0);
        m.set(static_cast<MD::Param>(MD::BelowThresholdLow + b), -80);
        m.set(static_cast<MD::Param>(MD::AttackLow + b), 10);
        m.set(static_cast<MD::Param>(MD::ReleaseLow + b), 100);
    }
}
struct Stereo {
    std::vector<float> l, r;
};
Stereo render(MD &m, const Stereo &in, std::size_t block = 64, const Stereo *sc = nullptr) {
    Stereo out{std::vector<float>(in.l.size()), std::vector<float>(in.l.size())};
    for (std::size_t i = 0; i < in.l.size(); i += block) {
        const auto n = std::min(block, in.l.size() - i);
        m.process(in.l.data() + i, in.r.data() + i, out.l.data() + i, out.r.data() + i, n,
                  sc ? sc->l.data() + i : nullptr, sc ? sc->r.data() + i : nullptr);
    }
    return out;
}
Stereo mono(std::vector<float> x) { return {x, x}; }
std::vector<float> dc(std::size_t n, double db) {
    return std::vector<float>(n, static_cast<float>(std::pow(10.0, db / 20.0)));
}
double db(double v) { return 20.0 * std::log10(std::abs(v)); }
double magnitude(const std::vector<float> &x, std::size_t from, double hz) {
    std::complex<double> sum = 0;
    for (std::size_t k = from; k < x.size(); ++k)
        sum += static_cast<double>(x[k]) *
               std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(k - from) / 48000);
    return std::abs(sum);
}

// HIIR's coefficient design, re-derived here so the constant in the core is checked.
std::array<double, 8> halfbandDesign() {
    const double tbw = 0.01;
    double k = std::tan((1 - tbw * 2) * std::numbers::pi / 4);
    k *= k;
    const double kk = std::pow(1 - k * k, 0.25), e = 0.5 * (1 - kk) / (1 + kk), e2 = e * e,
                 e4 = e2 * e2, q = e * (1 + e4 * (2 + e4 * (15 + 150 * e4)));
    std::array<double, 8> out{};
    const int order = 17;
    for (int i = 0; i < 8; ++i) {
        const int c = i + 1;
        double num = 0, den = 0;
        for (int j = 0, s = 1;; ++j, s = -s) {
            const double t = std::pow(q, j * (j + 1)) * std::sin((j * 2 + 1) * c * std::numbers::pi / order) * s;
            num += t;
            if (std::abs(t) <= 1e-100)
                break;
        }
        for (int j = 1, s = -1;; ++j, s = -s) {
            const double t = std::pow(q, j * j) * std::cos(j * 2 * c * std::numbers::pi / order) * s;
            den += t;
            if (std::abs(t) <= 1e-100)
                break;
        }
        const double ww = num * std::pow(q, 0.25) / (den + 0.5), w2 = ww * ww;
        const double x = std::sqrt((1 - w2 * k) * (1 - w2 / k)) / (1 + w2);
        out[static_cast<std::size_t>(i)] = (1 - x) / (1 + x);
    }
    return out;
}

void resampler() {
    const auto design = halfbandDesign();
    bool same = true;
    for (std::size_t i = 0; i < 8; ++i)
        same &= static_cast<float>(design[i]) == MD::kHalfbandCoefficients[i];
    check(same, "half-band coefficients are HIIR's 8-coefficient, 0.01-transition design");
    MD m;
    neutral(m);
    m.set(MD::LowBandOn, 0);
    m.set(MD::HighBandOn, 0);
    m.prepare(48000);
    std::vector<float> x(256);
    x[0] = 0.5f;
    const auto y = render(m, mono(x));
    bool exact = true;
    for (std::size_t i = 0; i < kLiveImpulse.size(); ++i)
        exact &= y.l[i] == kLiveImpulse[i] && y.r[i] == kLiveImpulse[i];
    check(exact, "single-band impulse response equals Live's render bit for bit");
    // 2x at any host rate: Live rendered at 44.1 and 96 kHz agrees just as exactly
    const auto atRate = [](double rate, const std::array<float, 32> &want) {
        MD r;
        neutral(r);
        r.set(MD::LowBandOn, 0);
        r.set(MD::HighBandOn, 0);
        r.prepare(rate);
        std::vector<float> in(64);
        in[0] = 0.5f;
        const auto got = render(r, mono(in));
        bool same = true;
        for (std::size_t i = 0; i < want.size(); ++i)
            same &= got.l[i] == want[i];
        return same;
    };
    check(atRate(44100, kLiveImpulse44k), "44.1 kHz single-band impulse equals Live's bit for bit");
    check(atRate(96000, kLiveImpulse96k), "96 kHz single-band impulse equals Live's bit for bit");
}

void crossovers() {
    struct Case {
        double low, high;
        int solo;
        const std::array<double, 7> *want;
        const char *what;
    } cases[] = {{120, 2500, 0, &k_n_soloLow, "low band 120 Hz matches Live"},
                 {120, 2500, 1, &k_n_soloMid, "mid band 120/2500 matches Live"},
                 {120, 2500, 2, &k_n_soloHigh, "high band 2500 Hz matches Live"},
                 {120, 2500, -1, &k_n_neutral, "neutral three-band sum matches Live"},
                 {1000, 8000, 0, &k_x1000_8000_soloLow, "low band 1 kHz matches Live"},
                 {1000, 8000, 1, &k_x1000_8000_soloMid, "mid band 1k/8k matches Live"},
                 {1000, 8000, 2, &k_x1000_8000_soloHigh, "high band 8 kHz matches Live"}};
    for (const auto &c : cases) {
        MD m;
        neutral(m);
        m.set(MD::LowMidCrossover, c.low);
        m.set(MD::MidHighCrossover, c.high);
        if (c.solo >= 0)
            m.set(static_cast<MD::Param>(MD::SoloLow + c.solo), 1);
        m.prepare(48000);
        std::vector<float> x(16384);
        x[0] = 0.5f;
        const auto y = render(m, mono(x));
        bool ok = true;
        for (std::size_t i = 0; i < kFreqs.size(); ++i) {
            const double got = db(magnitude(y.l, 0, kFreqs[i]) / 0.5), want = (*c.want)[i];
            // to a few thousandths of a dB in the passband; below -90 dB both are float noise
            const double tol = want > -60 ? 0.02 : 1.0;
            const bool bad = want > -90 ? std::abs(got - want) > tol : got > -80;
            if (bad) {
                ok = false;
                std::printf("     %s %.0f Hz: got %.4f want %.4f\n", c.what, kFreqs[i], got, want);
            }
        }
        check(ok, c.what);
    }
}

// Static curves: DC at a level, peak detection, 0.1 ms times, read once settled.
double staticGain(double level, void (*setup)(MD &)) {
    MD m;
    neutral(m);
    m.set(MD::LowBandOn, 0);
    m.set(MD::HighBandOn, 0);
    m.set(MD::PeakMode, 1);
    m.set(MD::AttackMid, 0.1);
    m.set(MD::ReleaseMid, 0.1);
    setup(m);
    m.prepare(48000);
    const auto y = render(m, mono(dc(4800, level)));
    return db(y.l.back()) - level;
}
void statics() {
    struct Case {
        double level, want;
        void (*setup)(MD &);
        const char *what;
    };
    const auto comp = [](MD &m) { m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75); };
    const auto knee = [](MD &m) {
        m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75), m.set(MD::SoftKnee, 1);
    };
    const auto expand = [](MD &m) { m.set(MD::BelowThresholdMid, -50), m.set(MD::BelowRatioMid, -3); };
    const auto upward = [](MD &m) {
        m.set(MD::BelowThresholdMid, -50), m.set(MD::BelowRatioMid, 0.5), m.set(MD::SoftKnee, 1);
    };
    const auto upx = [](MD &m) { m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, 1); };
    const auto crossed = [](MD &m) {
        m.set(MD::AboveThresholdMid, -40), m.set(MD::AboveRatioMid, -0.75);
        m.set(MD::BelowThresholdMid, -20), m.set(MD::BelowRatioMid, 0.5);
    };
    const auto amount = [](MD &m) {
        m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75), m.set(MD::Amount, 50);
    };
    const Case cases[] = {
        {-24, 0.0000, comp, "4:1 above -20 dB at -24 dB (Live)"},
        {-16, -3.0013, comp, "4:1 above -20 dB at -16 dB (Live)"},
        {-8, -9.0043, comp, "4:1 above -20 dB at -8 dB (Live)"},
        {0, -15.0022, comp, "4:1 above -20 dB at 0 dB (Live)"},
        {6, -19.5008, comp, "4:1 above -20 dB at 6 dB (Live)"},
        {-30, 0.0000, knee, "soft knee, 4:1 at -20 at -30 dB (Live)"},
        {-24, -0.6746, knee, "soft knee, 4:1 at -20 at -24 dB (Live)"},
        {-20, -1.8769, knee, "soft knee, 4:1 at -20 at -20 dB (Live)"},
        {-16, -3.6760, knee, "soft knee, 4:1 at -20 at -16 dB (Live)"},
        {-8, -9.0043, knee, "soft knee, 4:1 at -20 at -8 dB (Live)"},
        {-80, -89.9802, expand, "1:4 expansion below -50 at -80 dB (Live)"},
        {-60, -29.9927, expand, "1:4 expansion below -50 at -60 dB (Live)"},
        {-52, -5.9864, expand, "1:4 expansion below -50 at -52 dB (Live)"},
        {-62, 5.9967, upward, "upward compression with knee at -62 dB (Live)"},
        {-56, 3.1966, upward, "upward compression with knee at -56 dB (Live)"},
        {-50, 1.2492, upward, "upward compression with knee at -50 dB (Live)"},
        {-44, 0.2000, upward, "upward compression with knee at -44 dB (Live)"},
        {-10, 10.0004, upx, "1:2 upward expansion at -10 dB (Live)"},
        {0, 20.0019, upx, "1:2 upward expansion at 0 dB (Live)"},
        {-60, 20.0003, crossed, "crossed thresholds add at -60 dB (Live)"},
        {-30, -2.5027, crossed, "crossed thresholds add at -30 dB (Live)"},
        {-10, -22.5015, crossed, "crossed thresholds add at -10 dB (Live)"},
        {-10, -3.7506, amount, "Amount 50% halves r at -10 dB (Live)"},
        {0, -7.5004, amount, "Amount 50% halves r at 0 dB (Live)"},
    };
    for (const auto &c : cases)
        near(staticGain(c.level, c.setup), c.want, 0.01, c.what);
    // the cap: upward compression from far below asks for +60 dB and gets 64x
    near(staticGain(-140, [](MD &m) { m.set(MD::BelowThresholdMid, -40), m.set(MD::BelowRatioMid, 1); }),
         20 * std::log10(64.0), 0.002, "upward gain stops at 64x (+36.12 dB)");
    near(staticGain(-10, [](MD &m) { m.set(MD::AboveThresholdMid, -80), m.set(MD::AboveRatioMid, 1); }),
         20 * std::log10(64.0), 0.002, "upward expansion stops at 64x too");
    near(staticGain(-140, [](MD &m) {
             m.set(MD::BelowThresholdMid, -40), m.set(MD::BelowRatioMid, 1), m.set(MD::InputGainMid, 24);
         }),
         24 + 20 * std::log10(64.0), 0.002, "input gain sits outside the cap");
    near(staticGain(0, [](MD &m) { m.set(MD::AboveThresholdMid, -80), m.set(MD::AboveRatioMid, -1); }),
         -80, 0.01, "no floor on downward gain");
    near(staticGain(-10, [](MD &m) { m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -1), m.set(MD::Amount, 0); }),
         0, 1e-4, "Amount 0 is a ratio of 1");
}

// DC from -40 to -6 dB at 0.5 s and back at 1.5 s; threshold -20, 4:1, single band.
std::vector<double> ballistics(bool peak, double attack, double release, std::size_t t0,
                               std::initializer_list<double> ms) {
    std::vector<float> x(100000, static_cast<float>(std::pow(10.0, -40 / 20.0)));
    std::fill(x.begin() + 24000, x.begin() + 72000, static_cast<float>(std::pow(10.0, -6 / 20.0)));
    auto make = [&](bool compress) {
        MD m;
        neutral(m);
        m.set(MD::LowBandOn, 0);
        m.set(MD::HighBandOn, 0);
        m.set(MD::PeakMode, peak ? 1 : 0);
        m.set(MD::AttackMid, attack);
        m.set(MD::ReleaseMid, release);
        if (compress)
            m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75);
        m.prepare(48000);
        return render(m, mono(x)).l;
    };
    const auto y = make(true), ref = make(false);
    std::vector<double> out;
    for (double t : ms) {
        const auto i = t0 + static_cast<std::size_t>(std::lround(t * 48));
        out.push_back(db(y[i] / ref[i]));
    }
    return out;
}
void envelopes() {
    struct Case {
        bool peak;
        double attack, release;
        std::size_t t0;
        std::initializer_list<double> ms;
        std::vector<double> want;
        const char *what;
    };
    const Case cases[] = {
        {true, 10, 1, 24000, {1, 2, 5, 10}, {-7.0889, -9.3302, -10.4331, -10.5001}, "peak attack 10 ms (Live)"},
        {false, 10, 1, 24000, {1, 2, 5}, {-8.7779, -9.9173, -10.4679}, "RMS attack 10 ms (Live)"},
        {true, 100, 1, 24000, {5, 10, 20, 50}, {-4.1949, -7.2612, -9.3976, -10.4357}, "peak attack 100 ms (Live)"},
        {true, 0.1, 100, 72000, {5, 10, 15}, {-7.6938, -4.7339, -1.9064}, "peak release 100 ms (Live)"},
        {true, 0.1, 1000, 72000, {50, 100, 150}, {-7.5830, -4.7003, -1.8769}, "peak release 1000 ms (Live)"},
    };
    for (const auto &c : cases) {
        const auto got = ballistics(c.peak, c.attack, c.release, c.t0, c.ms);
        bool ok = true;
        for (std::size_t i = 0; i < got.size(); ++i)
            if (std::abs(got[i] - c.want[i]) > 0.03) {
                ok = false;
                std::printf("     %s #%zu: got %.4f want %.4f\n", c.what, i, got[i], c.want[i]);
            }
        check(ok, c.what);
    }
}

void stereoLink() {
    // Live: opposite-polarity channels compress as fully as in-phase ones (mean of |L|,|R|)
    for (int peak = 0; peak < 2; ++peak) {
        MD m;
        neutral(m);
        m.set(MD::LowBandOn, 0), m.set(MD::HighBandOn, 0), m.set(MD::PeakMode, peak);
        m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75);
        m.prepare(48000);
        auto l = dc(48000, -6), r = l;
        for (auto &v : r)
            v = -v;
        const auto y = render(m, {l, r});
        near(db(y.l.back() / l.back()), -10.5, 0.01, peak ? "peak link: polarity does not matter" : "RMS link: polarity does not matter");
        check(std::abs(y.l.back() + y.r.back()) < 1e-7f, "one gain for both channels");
    }
    MD m;
    neutral(m);
    m.set(MD::LowBandOn, 0), m.set(MD::HighBandOn, 0), m.set(MD::PeakMode, 1);
    m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75);
    m.prepare(48000);
    const auto y = render(m, {dc(48000, -6), dc(48000, -40)});
    near(db(y.l.back()) + 6, -6.113, 0.01, "peak link averages |L| and |R| (Live: -6.113 dB)");
    near(db(y.r.back()) + 40, -6.113, 0.01, "the quiet channel gets the same gain");
}

void sidechain() {
    // the trigger is cos/sin of Mix between the input and the sidechain, before the resampler
    MD m;
    neutral(m);
    m.set(MD::LowBandOn, 0), m.set(MD::HighBandOn, 0), m.set(MD::PeakMode, 1);
    m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75);
    m.set(MD::SidechainOn, 1), m.set(MD::SidechainMix, 50);
    m.prepare(48000);
    const auto in = mono(dc(48000, -30)), sc = mono(dc(48000, -6));
    const auto y = render(m, in, 64, &sc);
    near(db(y.l.back()) + 30, -8.646, 0.01, "Mix 50% is an equal-power blend (Live: -8.646 dB)");
    MD q;
    neutral(q);
    q.set(MD::LowBandOn, 0), q.set(MD::HighBandOn, 0), q.set(MD::SidechainOn, 1),
        q.set(MD::SidechainListen, 1), q.set(MD::SidechainGain, 20 * std::log10(2.0));
    q.prepare(48000);
    const auto heard = render(q, in, 64, &sc);
    near(heard.l.back(), 2 * std::pow(10.0, -6 / 20.0), 1e-5, "Listen plays the trigger, gain included");
    MD off;
    neutral(off);
    off.set(MD::AboveThresholdMid, -20), off.set(MD::AboveRatioMid, -0.75);
    off.set(MD::LowBandOn, 0), off.set(MD::HighBandOn, 0), off.set(MD::SidechainOn, 1),
        off.set(MD::SidechainMix, 0);
    off.prepare(48000);
    near(db(render(off, in, 64, &sc).l.back()) + 30, 0, 1e-4, "Mix 0% ignores the sidechain");
}

void routing() {
    std::vector<float> x(16384);
    x[0] = 0.5f;
    auto energy = [](const std::vector<float> &v) {
        double e = 0;
        for (float s : v)
            e += static_cast<double>(s) * s;
        return e;
    };
    for (double f : {2000.0, 500.0}) {
        MD m;
        neutral(m);
        m.set(MD::LowMidCrossover, f), m.set(MD::MidHighCrossover, 500), m.set(MD::SoloMid, 1);
        m.prepare(48000);
        check(energy(render(m, mono(x)).l) == 0, "low split at or above the high split: mid band silent");
    }
    MD crossed;
    neutral(crossed);
    crossed.set(MD::LowMidCrossover, 2000), crossed.set(MD::MidHighCrossover, 1000), crossed.set(MD::SoloLow, 1);
    crossed.prepare(48000);
    near(db(magnitude(render(crossed, mono(x)).l, 0, 1000) / 0.5), -6.02, 0.05, "crossed splits both sit at the high split");
    // crossed splits with one split switched off still clamp to the lower one (mbd_f2
    // c_lowonly_3000_300_soloLow and c_highonly_3000_300_soloHigh, both bit-exact)
    for (int off = 0; off < 2; ++off) {
        MD one;
        neutral(one);
        one.set(MD::LowMidCrossover, 3000), one.set(MD::MidHighCrossover, 300);
        one.set(off ? MD::LowBandOn : MD::HighBandOn, 0);
        one.set(off ? MD::SoloHigh : MD::SoloLow, 1);
        one.prepare(48000);
        near(db(magnitude(render(one, mono(x)).l, 0, 300) / 0.5), -6.02, 0.05,
             off ? "low split off: the high split stays at 300 Hz" : "high split off: the low split clamps to 300 Hz");
    }
    MD inactive;
    neutral(inactive);
    inactive.set(MD::OutputGainHigh, 6), inactive.set(MD::InputGainHigh, 6), inactive.set(MD::ActiveHigh, 0);
    inactive.prepare(48000);
    MD plain;
    neutral(plain);
    plain.prepare(48000);
    check(render(inactive, mono(x)).l == render(plain, mono(x)).l, "an inactive band ignores its gains");
    MD gone;
    neutral(gone);
    gone.set(MD::LowBandOn, 0), gone.set(MD::SoloLow, 1);
    gone.prepare(48000);
    check(energy(render(gone, mono(x)).l) == 0, "soloing a band that does not exist is silence");
    MD soloInactive;
    neutral(soloInactive);
    soloInactive.set(MD::SoloMid, 1), soloInactive.set(MD::ActiveMid, 0), soloInactive.set(MD::OutputGainMid, 6);
    soloInactive.prepare(48000);
    near(db(magnitude(render(soloInactive, mono(x)).l, 0, 1000) / 0.5), k_n_soloMid[3], 0.02, "a soloed inactive band plays raw");
}

void engineering() {
    // block size never changes a sample
    std::vector<float> x(48000);
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * 997 * static_cast<double>(i) / 48000) *
                                  (i % 9000 < 4500 ? 1.0 : 0.05));
    auto run = [&](std::size_t block) {
        MD m;
        m.set(MD::AboveRatioLow, -0.75), m.set(MD::AboveRatioMid, -0.5), m.set(MD::BelowRatioHigh, 0.5);
        m.prepare(48000);
        return render(m, mono(x), block);
    };
    const auto ref = run(64);
    bool same = true;
    for (std::size_t b : {1u, 32u, 333u, 4096u})
        same &= run(b).l == ref.l;
    check(same, "every block size renders the same samples");
    MD m;
    m.prepare(48000);
    std::vector<float> l(4096), r(4096), sl(4096), sr(4096), ol(4096), orr(4096);
    m.set(MD::SidechainOn, 1);
    allocations = 0;
    countAllocations = true;
    m.set(MD::MasterOutput, -6);
    m.process(l.data(), r.data(), ol.data(), orr.data(), l.size(), sl.data(), sr.data());
    countAllocations = false;
    check(allocations == 0, "set and process allocate nothing");
    // a parameter jump is Live's S-curve, two 94-sample boxcars: half way at 93, done at 187
    MD s;
    neutral(s);
    s.set(MD::LowBandOn, 0), s.set(MD::HighBandOn, 0);
    s.prepare(48000);
    std::vector<float> one(6000, 1.0f), out(6000), outR(6000);
    s.process(one.data(), one.data(), out.data(), outR.data(), 4800);
    s.set(MD::MasterOutput, -20);
    s.process(one.data() + 4800, one.data() + 4800, out.data() + 4800, outR.data() + 4800, 1200);
    near(db(out[4799]), 0, 1e-4, "before the jump");
    near(db(out[4800 + 93]), -10, 0.25, "half way after ~2 ms");
    near(db(out[4800 + 187]), -20, 1e-3, "settled after ~4 ms");
    MD clamp;
    clamp.set(MD::AttackLow, 1e9);
    clamp.set(MD::LowMidCrossover, std::nan(""));
    check(clamp.get(MD::AttackLow) == 5000 && clamp.get(MD::LowMidCrossover) == 120,
          "values clamp to Live's ranges and NaN is ignored");
}

// Live's crossover network to the bit: RBJ sections in float, direct form I, splits held
// as float log10, one biquad allpass per outer band, the mid band scaled.
void liveNetwork() {
    const auto impulse = [](double rate, std::size_t n, void (*setup)(MD &)) {
        MD m;
        neutral(m);
        setup(m);
        m.prepare(rate);
        std::vector<float> x(n);
        x[0] = 0.5f;
        return render(m, mono(x)).l;
    };
    check(sameAsLive(impulse(48000, 256, [](MD &) {}), kLiveNeutral3Band, 2e-6),
          "three-band neutral impulse response is Live's");
    check(sameAsLive(impulse(48000, 256, [](MD &m) {
              m.set(MD::LowMidCrossover, 30), m.set(MD::MidHighCrossover, 300), m.set(MD::SoloLow, 1);
          }),
          kLiveLow30Hz, 2e-6),
          "30 Hz low band, Live's float artefacts included");
    check(sameAsLive(impulse(96000, 256, [](MD &) {}), kLiveNeutral96k, 2e-6),
          "three-band neutral at 96 kHz (192 kHz inside) is Live's");
}

void knobs() {
    struct Knob {
        double db;
        float live;
        const char *what;
    } knobs[] = {{11.42, 0x1.dca954p+1f, "input gain 11.42 dB (mbd_gain): powf(10, dB*0.05f)"},
                 {5.87, 0x1.f733p+0f, "output gain 5.87 dB (mbd_gain)"},
                 {-2.13, 0x1.90a788p-1f, "master -2.13 dB (mbd_gain)"}};
    for (const auto &k : knobs) {
        const float got = MD::knobGain(k.db);
        check(kLiveLibm ? got == k.live : std::abs(got / k.live - 1) < 3e-7, k.what);
        if (kLiveLibm && got != k.live)
            std::printf("     got %a want %a\n", static_cast<double>(got), static_cast<double>(k.live));
    }
}

// Single band, Listen on, S/C on: what the detector hears plays, through the resampler.
std::vector<float> listened(void (*setup)(MD &), double main, double side, std::size_t n = 9600) {
    MD m;
    neutral(m);
    m.set(MD::LowBandOn, 0), m.set(MD::HighBandOn, 0);
    m.set(MD::SidechainOn, 1), m.set(MD::SidechainListen, 1);
    setup(m);
    m.prepare(48000);
    const Stereo sc = mono(std::vector<float>(n, static_cast<float>(side)));
    return render(m, mono(std::vector<float>(n, static_cast<float>(main))), 64, &sc).l;
}
void sidechainLaw() {
    // Mix 99.9 %: the dry share is sqrtf(1 - wet^2), not cos -- Live measured 0.00158221
    // where the equal-power cos would be 0.00157080 (mbd_rt rt_mix0.999)
    const double resampler = 0.9999974; // the HIIR chain's own DC gain, measured
    const auto y = listened([](MD &m) { m.set(MD::SidechainMix, 99.9); }, 0.5, 0.0);
    near(y.back() / (0.5 * resampler), 0.00158221, 2e-8, "S/C mix: dry = sqrtf(1 - wet^2) (Live 0.00158221)");
    // the -70 dB end is off: -69.9 dB is silent in Live, -69.5 dB is not
    check(listened([](MD &m) { m.set(MD::SidechainGain, -69.8); }, 0.0, 0.5).back() == 0,
          "S/C gain just above -70 dB is off (Live: silent at -69.9 dB)");
    check(listened([](MD &m) { m.set(MD::SidechainGain, -69.5); }, 0.0, 0.5).back() > 0,
          "S/C gain at -69.5 dB is on");
    // Listen plays whenever it is on: with S/C off, the main input's bands, uncompressed
    // (mbd_rt rt_scoff_listen_single: Live's output there is not the normal output)
    MD q;
    neutral(q);
    q.set(MD::LowBandOn, 0), q.set(MD::HighBandOn, 0), q.set(MD::PeakMode, 1);
    q.set(MD::AboveThresholdMid, -40), q.set(MD::AboveRatioMid, -1), q.set(MD::SidechainListen, 1);
    q.prepare(48000);
    const auto heard = render(q, mono(dc(9600, -12))).l;
    near(db(heard.back()) + 12, 0, 1e-3, "Listen with S/C off plays the main, uncompressed");
}

// S/C On moves the trigger from the main to the sidechain over 1.5 ms of smoothstep:
// 72 samples at 48 kHz, 66 at 44.1 (mbd_audit scsw_dc_listen, sr44/sr96 variants).
void sidechainRamp() {
    for (double rate : {48000.0, 44100.0}) {
        MD m;
        neutral(m);
        m.set(MD::LowBandOn, 0), m.set(MD::HighBandOn, 0), m.set(MD::SidechainListen, 1);
        m.prepare(rate);
        const std::size_t n = 4096;
        std::vector<float> main(n, 0.25f), side(n, 0.02f), out(n), outR(n);
        m.process(main.data(), main.data(), out.data(), outR.data(), 2048, side.data(), side.data());
        m.set(MD::SidechainOn, 1);
        m.process(main.data() + 2048, main.data() + 2048, out.data() + 2048, outR.data() + 2048,
                  n - 2048, side.data() + 2048, side.data() + 2048);
        // the expected trigger, run through the same resampler
        const auto length = static_cast<std::size_t>(std::lround(rate * 0.0015));
        // Listen played the main through its own upsampler until the switch; from the
        // switch on it plays the sidechain path, whose upsampler starts cold (Live: the
        // ringing at the first switch-on is reproduced to the bit, scsw_dc_listen)
        MD::HalfbandUp mainUp, scUp;
        MD::HalfbandDown down;
        for (std::size_t i = 0; i < 4; ++i) {
            mainUp.even.c[i] = scUp.even.c[i] = down.even.c[i] = MD::kHalfbandCoefficients[2 * i];
            mainUp.odd.c[i] = scUp.odd.c[i] = down.odd.c[i] = MD::kHalfbandCoefficients[2 * i + 1];
        }
        double worst = 0;
        for (std::size_t i = 0; i < n; ++i) {
            double s = 0;
            if (i >= 2048) {
                const double t = std::min(1.0, static_cast<double>(i - 2048) / static_cast<double>(length));
                s = t * t * (3 - 2 * t);
            }
            const auto trigger = static_cast<float>((1 - s) * 0.25 + s * 0.02);
            float a, b;
            (i < 2048 ? mainUp : scUp).step(trigger, a, b);
            const float want = down.step(a, b);
            worst = std::max(worst, std::abs(static_cast<double>(out[i]) - want));
        }
        check(worst < 2e-6, rate == 48000 ? "S/C On ramp: 72-sample smoothstep at 48 kHz"
                                          : "S/C On ramp: 66 samples at 44.1 kHz (1.5 ms)");
        if (worst >= 2e-6)
            std::printf("     worst %.3g\n", worst);
    }
}

// Flipping Peak/RMS mid-stream carries the envelope over: Live's output does not move
// (mbd_d auto_rms_to_peak / auto_peak_to_rms, mbd_audit pksw_*).
void peakRmsSwitch() {
    MD m;
    neutral(m);
    m.set(MD::LowBandOn, 0), m.set(MD::HighBandOn, 0), m.set(MD::PeakMode, 1);
    m.set(MD::AboveThresholdMid, -32), m.set(MD::AboveRatioMid, -1);
    m.prepare(48000);
    const auto x = dc(24000, -12);
    std::vector<float> a(24000), ar(24000), b(24000), br(24000);
    m.process(x.data(), x.data(), a.data(), ar.data(), 24000);
    m.set(MD::PeakMode, 0);
    m.process(x.data(), x.data(), b.data(), br.data(), 24000);
    // the oversampled pair alternates two values; the switch continues the pair as is
    check(b[0] == a[a.size() - 2] && b[1] == a.back(),
          "Peak -> RMS keeps the gain (the envelope is squared, not restarted)");
}

// The detector starts at 0.01 in its own units and updates as c*env + (1-c)*d with
// c*env rounded on its own: Live's samples from battery D.
void detectorState() {
    {
        MD m;
        neutral(m);
        m.set(MD::LowBandOn, 0), m.set(MD::HighBandOn, 0), m.set(MD::PeakMode, 1);
        m.set(MD::BelowThresholdMid, -30), m.set(MD::BelowRatioMid, 1);
        m.set(MD::AttackMid, 1), m.set(MD::ReleaseMid, 5000);
        m.prepare(48000);
        std::vector<float> x(48000, 0.0f);
        std::fill(x.begin() + 2400, x.end(), static_cast<float>(std::pow(10.0, -60 / 20.0)));
        const auto y = render(m, mono(x)).l;
        const std::array<std::size_t, 4> at{2400 + 48, 2400 + 480, 2400 + 4800, 2400 + 24000};
        std::vector<float> got;
        for (auto i : at)
            got.push_back(y[i]);
        check(sameAsLive(got, kLiveInitUpward, 1e-8), "envelope starts at 0.01 (Live, init_lead0.05_up_pk)");
    }
    {
        MD m;
        neutral(m);
        m.set(MD::LowBandOn, 0), m.set(MD::HighBandOn, 0), m.set(MD::PeakMode, 1);
        m.set(MD::AboveThresholdMid, -80), m.set(MD::AboveRatioMid, -0.5);
        m.set(MD::AttackMid, 0.1), m.set(MD::ReleaseMid, 5000);
        m.prepare(48000);
        std::vector<float> x(48000 * 6, static_cast<float>(std::pow(10.0, -80 / 20.0)));
        std::fill(x.begin(), x.begin() + 48000, static_cast<float>(std::pow(10.0, -6 / 20.0)));
        // as Live played it: a 5 s release creeps a few ulps a step, so even the clip's
        // 4 ms fade-in a second earlier decides the path (1.5 % apart without it)
        for (std::size_t i = 0; i < kLiveClipFade.size(); ++i)
            x[i] *= kLiveClipFade[i];
        const auto y = render(m, mono(x)).l;
        const std::array<std::size_t, 4> at{48000 + 480, 48000 + 4800, 48000 + 48000, 48000 + 192000};
        std::vector<float> got;
        for (auto i : at)
            got.push_back(y[i]);
        check(sameAsLive(got, kLiveRelease5000, 1e-9), "5 s release, Live's rounding (coef_rel5000_pk)");
    }
}

// Bit-level static curve: DC stairs (100 ms lead, 150 ms per 2 dB from -84), peak/RMS,
// 0.1 ms times, 4:1 above -20 dB; Live's own output mid-stair.
std::vector<float> stairsAt(bool peak, bool knee, std::initializer_list<double> levels) {
    std::vector<float> x(4800 + 46 * 7200);
    for (int i = 0; i < 46; ++i)
        std::fill(x.begin() + 4800 + i * 7200, x.begin() + 4800 + (i + 1) * 7200,
                  static_cast<float>(std::pow(10.0, (-84 + 2 * i) / 20.0)));
    MD m;
    neutral(m);
    m.set(MD::LowBandOn, 0), m.set(MD::HighBandOn, 0), m.set(MD::PeakMode, peak ? 1 : 0);
    m.set(MD::SoftKnee, knee ? 1 : 0), m.set(MD::AttackMid, 0.1), m.set(MD::ReleaseMid, 0.1);
    m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75);
    m.prepare(48000);
    const auto y = render(m, mono(x)).l;
    std::vector<float> got;
    for (double level : levels)
        got.push_back(y[4800 + static_cast<std::size_t>(std::lround((level + 84) / 2)) * 7200 + 3600]);
    return got;
}
void gainComputerBits() {
    check(sameAsLive(stairsAt(true, true, {-28, -24, -20, -16}), kLiveKneeStairs, 1e-9),
          "soft knee to the bit (st_above-20_r-0.75_peak_knee)");
    check(sameAsLive(stairsAt(false, false, {-16, -8, 0, 6}), kLiveRmsStairs, 1e-9),
          "RMS level through Live's rsqrt/recip estimates (st_above-20_r-0.75_rms)");
    // the mid band's scale at 2161/2848 Hz: the g that nulls mbd_f g_2161_2848_soloMid
    MD::BandSplit split;
    split.design(2161, 2848, 96000);
    check(kLiveLibm ? split.midGain == 0x1.55a39cp-1f : std::abs(split.midGain / 0x1.55a39cp-1f - 1) < 3e-7,
          "mid band gain 1 - 10^(-24 dB/oct * octaves * 0.05f) (g_2161_2848_soloMid)");
}

void silenceAndDenormals() {
    // Live renders with denormals flushed: none of its outputs holds a subnormal, and a
    // 30 Hz low band's decaying tail goes to exact zero (x30_soloLow_imp1e-6)
    MD m;
    neutral(m);
    m.set(MD::LowMidCrossover, 30), m.set(MD::MidHighCrossover, 300), m.set(MD::SoloLow, 1);
    m.prepare(48000);
    std::vector<float> x(48000 * 4);
    x[0] = 1e-30f;
    const auto y = render(m, mono(x)).l;
    bool subnormal = false;
    for (float v : y)
        subnormal |= std::fpclassify(v) == FP_SUBNORMAL;
    check(!subnormal && y.back() == 0, "denormals flushed: a 30 Hz tail ends in exact zero");
    // Listen with every band inactive plays nothing at all (lis_3b_inactAll: exactly 0)
    MD q;
    neutral(q);
    q.set(MD::ActiveLow, 0), q.set(MD::ActiveMid, 0), q.set(MD::ActiveHigh, 0);
    q.set(MD::SidechainOn, 1), q.set(MD::SidechainListen, 1);
    q.prepare(48000);
    std::vector<float> noise(9600);
    for (std::size_t i = 0; i < noise.size(); ++i)
        noise[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.37) * 0.3);
    const Stereo sc = mono(noise);
    const auto heard = render(q, mono(noise), 64, &sc).l;
    check(std::all_of(heard.begin(), heard.end(), [](float v) { return v == 0; }),
          "Listen with every band inactive is silent");
}
} // namespace
int main() {
    resampler();
    crossovers();
    statics();
    envelopes();
    stereoLink();
    sidechain();
    routing();
    engineering();
    liveNetwork();
    knobs();
    sidechainLaw();
    sidechainRamp();
    peakRmsSwitch();
    detectorState();
    gainComputerBits();
    silenceAndDenormals();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
