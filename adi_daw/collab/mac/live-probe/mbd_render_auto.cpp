// Offline renderer for the Multiband Dynamics core with switch automation:
//   mbd_auto in.wav out.wav [-s sc.wav] [-n len] [-p Name=value]... [-e <sample>:<Name>=<value>]...
// An -e event takes effect from that sample on (applied before it is processed). "DeviceOn" is
// the host's device On switch, replayed the way Live 11 does it (see main()); every other name
// is a parameter of the core, set at its sample.
#include <adi/dsp/multiband_dynamics.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
using MD = adi::dsp::MultibandDynamics;
static const char *kNames[] = {"LowMidCrossover","MidHighCrossover","SoftKnee","PeakMode","MasterOutput","Amount","TimeScaling",
 "OutputGainLow","OutputGainMid","OutputGainHigh","InputGainLow","InputGainMid","InputGainHigh","ActiveLow","ActiveMid","ActiveHigh",
 "AboveThresholdLow","AboveThresholdMid","AboveThresholdHigh","BelowThresholdLow","BelowThresholdMid","BelowThresholdHigh",
 "AboveRatioLow","AboveRatioMid","AboveRatioHigh","BelowRatioLow","BelowRatioMid","BelowRatioHigh","AttackLow","AttackMid","AttackHigh",
 "ReleaseLow","ReleaseMid","ReleaseHigh","SidechainOn","SidechainGain","SidechainMix","LowBandOn","HighBandOn","SoloLow","SoloMid","SoloHigh","SidechainListen"};
static bool readWav(const char *path, std::vector<float> &l, std::vector<float> &r, int &sr) {
    std::ifstream f(path, std::ios::binary); if (!f) return false;
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    size_t i = 12; int ch = 0, bits = 0, tag = 0; const char *data = nullptr; uint32_t dsz = 0;
    while (i + 8 <= b.size()) {
        uint32_t sz; std::memcpy(&sz, b.data() + i + 4, 4);
        if (!std::memcmp(b.data() + i, "fmt ", 4)) {
            uint16_t t, c, bp; uint32_t s; std::memcpy(&t, b.data() + i + 8, 2); std::memcpy(&c, b.data() + i + 10, 2);
            std::memcpy(&s, b.data() + i + 12, 4); std::memcpy(&bp, b.data() + i + 22, 2); tag = t; ch = c; sr = (int)s; bits = bp;
            if (tag == 0xFFFE) { uint16_t st; std::memcpy(&st, b.data() + i + 32, 2); tag = st; }
        } else if (!std::memcmp(b.data() + i, "data", 4)) { data = b.data() + i + 8; dsz = sz; }
        i += 8 + sz + (sz & 1);
    }
    if (!data || !(bits == 32 && tag == 3)) return false;
    size_t n = dsz / (4 * ch); l.resize(n); r.resize(n);
    for (size_t k = 0; k < n; ++k) { std::memcpy(&l[k], data + (k * ch) * 4, 4); std::memcpy(&r[k], data + (k * ch + (ch > 1 ? 1 : 0)) * 4, 4); }
    return true;
}
static void writeWav(const char *path, const std::vector<float> &l, const std::vector<float> &r, int sr) {
    std::ofstream f(path, std::ios::binary); uint32_t n = (uint32_t)l.size(), dsz = n * 8, rs = 36 + dsz, fs = 16, br = sr * 8;
    uint16_t t = 3, c = 2, ba = 8, bp = 32; uint32_t s = sr;
    f.write("RIFF", 4); f.write((char *)&rs, 4); f.write("WAVEfmt ", 8); f.write((char *)&fs, 4); f.write((char *)&t, 2); f.write((char *)&c, 2);
    f.write((char *)&s, 4); f.write((char *)&br, 4); f.write((char *)&ba, 2); f.write((char *)&bp, 2); f.write("data", 4); f.write((char *)&dsz, 4);
    for (uint32_t k = 0; k < n; ++k) { f.write((char *)&l[k], 4); f.write((char *)&r[k], 4); }
}
int main(int argc, char **argv) {
    if (argc < 3) { std::fprintf(stderr, "usage\n"); return 2; }
    std::vector<float> l, r, sl, sr2; int rate = 0, srate = 0;
    if (!readWav(argv[1], l, r, rate)) { std::fprintf(stderr, "bad input\n"); return 1; }
    MD m; std::map<std::string, int> idx; for (int i = 0; i < MD::Count; ++i) idx[kNames[i]] = i;
    idx["DeviceOn"] = -1;
    std::vector<std::pair<int, double>> ps; size_t len = l.size();
    struct Ev { size_t at; int p; double v; };
    std::vector<Ev> evs;
    for (int a = 3; a < argc; ++a) {
        if (!std::strcmp(argv[a], "-s")) { if (!readWav(argv[++a], sl, sr2, srate)) return 1; }
        else if (!std::strcmp(argv[a], "-n")) { len = std::stoul(argv[++a]); }
        else if (!std::strcmp(argv[a], "-p") || !std::strcmp(argv[a], "-e")) {
            const bool ev = argv[a][1] == 'e'; std::string kv = argv[++a]; size_t at = 0;
            if (ev) { auto c = kv.find(':'); at = std::stoul(kv.substr(0, c)); kv = kv.substr(c + 1); }
            auto e = kv.find('='); auto it = idx.find(kv.substr(0, e));
            if (it == idx.end()) { std::fprintf(stderr, "unknown %s\n", kv.c_str()); return 1; }
            if (ev) evs.push_back({at, it->second, std::stod(kv.substr(e + 1))});
            else ps.push_back({it->second, std::stod(kv.substr(e + 1))}); }
        else { std::fprintf(stderr, "unknown option %s\n", argv[a]); return 1; }
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev &a, const Ev &b) { return a.at < b.at; });
    bool wanted = true; // the device On switch as automated
    for (auto &[p, v] : ps) { if (p < 0) wanted = v >= 0.5; else m.set((MD::Param)p, v); }
    size_t ei = 0; // events at sample 0 are initial values
    for (; ei < evs.size() && evs[ei].at == 0; ++ei) { if (evs[ei].p < 0) wanted = evs[ei].v >= 0.5; else m.set((MD::Param)evs[ei].p, evs[ei].v); }
    m.prepare(rate);
    l.resize(len, 0.0f); r.resize(len, 0.0f); if (!sl.empty()) { sl.resize(len, 0.0f); sr2.resize(len, 0.0f); }
    std::vector<float> ol(len), orr(len);
    // Live's device On switch (devsw_*, sr44_/sr96_devsw_sine, auto_devon_*): a switch crossfades
    // wet and dry over 1 ms in whole samples (48, 96 and 44 at 48, 96 and 44.1 kHz), from the
    // switch's own sample: out = g*wet + (1-g)*dry in float, g = 0.5*(1 + cos(pi*k/fade)) to Off
    // and 1 - that to On (k = 0 at the switch), rounded to float, the dry gain 1 - g in float.
    // That is exact on about 80 % of the fade samples; the others are an ulp or two off (how
    // Live gets the last bit of g is not known: -176 dB in auto_devon_*). The device runs during
    // a fade; once a fade to Off has ended it is not processed at all (its state freezes) and
    // the output is the dry input. A switch that arrives during a fade waits for it to end and
    // then fades back at once (devsw_dc_short: Off at 48000, On at 48020 -> fade-out to 48048,
    // fade-in from 48048). Switching On after the device has been skipped calls
    // resumeAfterBypass() (detectors back to their start, crossovers cleared, resampler kept);
    // without a skipped sample there is no reset (devsw_dc_short: still fully compressed).
    const long fade = std::max(1, rate / 1000);
    bool on = wanted, skipped = false;
    long fadePos = fade; // samples into the running fade, == fade when none runs
    size_t i = 0;
    while (i < len) {
        for (; ei < evs.size() && evs[ei].at <= i; ++ei) {
            if (evs[ei].p < 0) wanted = evs[ei].v >= 0.5; else m.set((MD::Param)evs[ei].p, evs[ei].v);
        }
        if (fadePos >= fade && wanted != on) {
            on = wanted;
            fadePos = 0;
            if (on && skipped) m.resumeAfterBypass();
            skipped = false;
        }
        const float *scL = sl.empty() ? nullptr : sl.data() + i, *scR = sl.empty() ? nullptr : sr2.data() + i;
        if (fadePos < fade) { // sample by sample through a fade
            m.process(l.data() + i, r.data() + i, ol.data() + i, orr.data() + i, 1, scL, scR);
            const double c = 0.5 * (1.0 + std::cos(M_PI * static_cast<double>(fadePos) / static_cast<double>(fade)));
            const float g = static_cast<float>(on ? 1.0 - c : c), d = 1.0f - g;
            ol[i] = g * ol[i] + d * l[i];
            orr[i] = g * orr[i] + d * r[i];
            ++fadePos;
            ++i;
            continue;
        }
        size_t end = ei < evs.size() ? std::min(len, evs[ei].at) : len;
        end = std::min(end, i + 4096);
        if (on) m.process(l.data() + i, r.data() + i, ol.data() + i, orr.data() + i, end - i, scL, scR);
        else {
            std::copy(l.begin() + i, l.begin() + end, ol.begin() + i);
            std::copy(r.begin() + i, r.begin() + end, orr.begin() + i);
            skipped = true;
        }
        i = end;
    }
    writeWav(argv[2], ol, orr, rate);
    return 0;
}
