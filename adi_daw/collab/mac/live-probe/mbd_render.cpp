// Offline renderer for the Multiband Dynamics core: mbd_render in.wav out.wav [-s sc.wav] [-p Name=value]...
#include "adi/dsp/multiband_dynamics.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
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
    std::vector<std::pair<int, double>> ps; size_t len = l.size(); int block = 64;
    for (int a = 3; a < argc; ++a) {
        if (!std::strcmp(argv[a], "-s")) { if (!readWav(argv[++a], sl, sr2, srate)) return 1; }
        else if (!std::strcmp(argv[a], "-n")) { len = std::stoul(argv[++a]); }
        else if (!std::strcmp(argv[a], "-b")) { block = std::stoi(argv[++a]); }
        else if (!std::strcmp(argv[a], "-p")) { std::string kv = argv[++a]; auto e = kv.find('='); auto it = idx.find(kv.substr(0, e));
            if (it == idx.end()) { std::fprintf(stderr, "unknown %s\n", kv.c_str()); return 1; } ps.push_back({it->second, std::stod(kv.substr(e + 1))}); }
    }
    for (auto &[p, v] : ps) m.set((MD::Param)p, v);
    m.prepare(rate);
    l.resize(len, 0.0f); r.resize(len, 0.0f); if (!sl.empty()) { sl.resize(len, 0.0f); sr2.resize(len, 0.0f); }
    std::vector<float> ol(len), orr(len);
    for (size_t i = 0; i < len; i += block) { size_t n = std::min<size_t>(block, len - i);
        m.process(l.data() + i, r.data() + i, ol.data() + i, orr.data() + i, n, sl.empty() ? nullptr : sl.data() + i, sl.empty() ? nullptr : sr2.data() + i); }
    writeWav(argv[2], ol, orr, rate);
    return 0;
}
