// SPDX-License-Identifier: MIT
//
// wtgen -- describe a wavetable in a few dozen numbers, then generate a new one
// from those numbers and a seed. Prototype for the in-plugin generator (ADR-0009).
//
// Original code; it copies nothing from Surge, so it carries the policy default
// (MIT, OPEN_SOURCE_POLICY.md section 1) and can move into the GPLv3 plugin as is.
//
//   wtgen analyze  <in.wav>                           descriptor JSON on stdout
//   wtgen generate <desc.json> <out.wav> [--seed N]   a table from a descriptor alone
//   wtgen random   <out.wav> [--seed N] [--frames N]  a table from random descriptors
//   wtgen pack     <ref-dir> <out-dir> [--seed N] [--report <file>]
//   wtgen selftest                                    checks the analyzer on known shapes
//
// THE BOUNDARY. `pack` makes a table "like" each reference without copying it.
// The only thing that crosses from a reference to its generated counterpart is a
// TableDesc: the frame count, one "motion" figure, and thirteen coarse numbers
// at each of three keypoints. Examples are brightness, spectral slope, odd/even
// balance, bandwidth, two formant bumps and level. That is 41 numbers against
// the reference's 4,096 to 71,680 samples. generate() takes a TableDesc and a seed
// and cannot see a sample of the reference. Every harmonic amplitude and phase
// it writes comes from its own maths and its own random fields. The reference
// audio is read a second time only to VERIFY: to report how close the new
// table's descriptors came, and to show the new table is not a copy.
//
// Output is a Serum-style wavetable, 2048 samples a frame, 32-bit float mono.
// It carries a `clm ` chunk ("<!>2048 ...", which Surge, Serum and Vital read)
// and Surge's own `srge` chunk. Surge's exporter writes the same pair
// (src/common/WAVFileSupport.cpp).
//
// Random numbers come from splitmix64, not <random>. The standard library's
// distributions are implementation-defined, so the same seed would give
// different tables on MSVC and on libc++. Anything that stores a seed in a patch
// needs the same table on every platform.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kN = 2048;          // samples per frame
constexpr int kMaxH = kN / 2 - 1; // highest harmonic written (1023)
constexpr double kPi = 3.14159265358979323846;
constexpr double kFloorDb = -120.0;

// --- deterministic randomness ---------------------------------------------------

struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next()
    {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uni() { return double(next() >> 11) * (1.0 / 9007199254740992.0); } // [0,1)
    double range(double lo, double hi) { return lo + (hi - lo) * uni(); }
};

uint64_t hashString(const std::string &s, uint64_t seed)
{
    uint64_t h = 1469598103934665603ull ^ seed;
    for (unsigned char c : s)
        h = (h ^ c) * 1099511628211ull;
    return h;
}

// --- FFT (radix-2, in place) ------------------------------------------------------

using cd = std::complex<double>;

void fft(std::vector<cd> &a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * kPi / double(len) * (inverse ? 1.0 : -1.0);
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len)
        {
            cd w(1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j)
            {
                const cd u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto &x : a)
            x /= double(n);
}

// Harmonic amplitudes a[1..kMaxH] of one frame; a[0] is unused.
std::vector<double> harmonics(const float *frame)
{
    std::vector<cd> buf(kN);
    for (int i = 0; i < kN; ++i)
        buf[size_t(i)] = cd(frame[i], 0.0);
    fft(buf, false);
    std::vector<double> a(kMaxH + 1, 0.0);
    for (int k = 1; k <= kMaxH; ++k)
        a[size_t(k)] = 2.0 * std::abs(buf[size_t(k)]) / double(kN);
    return a;
}

// sum_k a[k] sin(2 pi k n / N + phi[k])
std::vector<float> synthesize(const std::vector<double> &a, const std::vector<double> &phi)
{
    std::vector<cd> buf(kN, cd(0.0, 0.0));
    for (int k = 1; k <= kMaxH; ++k)
    {
        const cd c = std::polar(a[size_t(k)] / 2.0, phi[size_t(k)] - kPi / 2.0);
        buf[size_t(k)] = c;
        buf[size_t(kN - k)] = std::conj(c);
    }
    fft(buf, true);
    std::vector<float> out(kN);
    for (int i = 0; i < kN; ++i)
        out[size_t(i)] = float(buf[size_t(i)].real() * double(kN));
    return out;
}

double toDb(double x) { return x > 0.0 ? std::max(kFloorDb, 20.0 * std::log10(x)) : kFloorDb; }

// --- WAV in and out ---------------------------------------------------------------

struct Table
{
    std::vector<std::vector<float>> frames;
    double sampleRate = 44100.0;
};

uint32_t rd32(const unsigned char *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t rd16(const unsigned char *p) { return uint16_t(p[0] | p[1] << 8); }

bool readTable(const fs::path &path, Table &t, std::string &err)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
        return err = "not a RIFF/WAVE file", false;

    uint16_t tag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    int frameSize = 0;
    const unsigned char *pcm = nullptr;
    size_t pcmBytes = 0;
    for (size_t pos = 12; pos + 8 <= d.size();)
    {
        const unsigned char *id = d.data() + pos;
        const size_t size = rd32(d.data() + pos + 4);
        const unsigned char *body = d.data() + pos + 8;
        if (pos + 8 + size > d.size())
            break;
        if (!std::memcmp(id, "fmt ", 4) && size >= 16)
        {
            tag = rd16(body);
            channels = rd16(body + 2);
            rate = rd32(body + 4);
            bits = rd16(body + 14);
        }
        else if (!std::memcmp(id, "data", 4))
        {
            pcm = body;
            pcmBytes = size;
        }
        else if (!std::memcmp(id, "clm ", 4) && size >= 7 && body[0] == '<')
            frameSize = std::atoi(reinterpret_cast<const char *>(body) + 3);
        else if (!std::memcmp(id, "srge", 4) && size >= 8)
            frameSize = int(rd32(body + 4));
        pos += 8 + size + (size & 1);
    }
    if (!pcm || channels == 0)
        return err = "no fmt or data chunk", false;
    if (frameSize == 0)
        frameSize = kN;
    if (frameSize != kN)
        return err = "frame size " + std::to_string(frameSize) + " (only 2048 is supported)", false;

    const size_t bytesPer = bits / 8u, stride = bytesPer * channels;
    const size_t samples = pcmBytes / stride;
    if (samples < size_t(kN) || samples % size_t(kN) != 0)
        return err = "sample count " + std::to_string(samples) + " is not a whole number of frames", false;

    std::vector<float> mono(samples);
    for (size_t i = 0; i < samples; ++i)
    {
        const unsigned char *p = pcm + i * stride; // channel 0
        if (tag == 3 && bits == 32)
        {
            float v;
            std::memcpy(&v, p, 4);
            mono[i] = v;
        }
        else if (tag == 1 && bits == 16)
            mono[i] = float(int16_t(rd16(p))) / 32768.0f;
        else if (tag == 1 && bits == 24)
            mono[i] = float(int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8) / 8388608.0f;
        else if (tag == 1 && bits == 32)
            mono[i] = float(double(int32_t(rd32(p))) / 2147483648.0);
        else
            return err = "unsupported sample format", false;
    }
    t.sampleRate = rate;
    t.frames.assign(samples / size_t(kN), std::vector<float>(kN));
    for (size_t fr = 0; fr < t.frames.size(); ++fr)
        std::copy_n(mono.begin() + std::ptrdiff_t(fr * size_t(kN)), kN, t.frames[fr].begin());
    return true;
}

bool writeTable(const fs::path &path, const Table &t)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };

    std::string clm = "<!>2048 10000000 wavetable adi-surge wtgen";
    clm.push_back('\0');
    const uint32_t clmPad = uint32_t(clm.size() & 1);
    const uint32_t dataBytes = uint32_t(t.frames.size() * size_t(kN) * 4);
    const uint32_t riff = 4 + (8 + 16) + (8 + 8) + (8 + uint32_t(clm.size()) + clmPad) + (8 + dataBytes);

    f.write("RIFF", 4);
    w32(riff);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    w32(16);
    w16(3); // IEEE float
    w16(1);
    w32(uint32_t(t.sampleRate));
    w32(uint32_t(t.sampleRate) * 4);
    w16(4);
    w16(32);
    f.write("srge", 4);
    w32(8);
    w32(1);
    w32(uint32_t(kN));
    f.write("clm ", 4);
    w32(uint32_t(clm.size()));
    f.write(clm.data(), std::streamsize(clm.size()));
    if (clmPad)
        f.put('\0');
    f.write("data", 4);
    w32(dataBytes);
    for (const auto &fr : t.frames)
        f.write(reinterpret_cast<const char *>(fr.data()), std::streamsize(fr.size() * 4));
    return bool(f);
}

// --- descriptors -------------------------------------------------------------------

// Thirteen coarse numbers about one frame. Nothing here can rebuild the frame.
struct FrameDesc
{
    double rms = 0, peak = 0;  // level
    double centroid = 1;       // power-weighted mean harmonic number: brightness
    double slope = -6;         // dB per octave, octave-weighted least-squares fit
    double oddRatio = 0.5;     // share of power in odd harmonics: 1 = hollow/square-like
    double bandwidth = 1;      // highest harmonic within 60 dB of the loudest
    double f1Pos = 0, f1Gain = 0; // strongest bump above the slope: log2(harmonic), dB
    double f2Pos = 0, f2Gain = 0; // second bump, at least an octave away
    double roughness = 0;      // dB spread of the fine ripple once the bumps are removed
    double density = 1;        // share of harmonics present at all (low = sparse, chord-like)
    double crest = 1.4;        // peak / rms of the waveform: phase alignment
};

struct TableDesc
{
    int frames = 1;
    double motion = 0; // mean dB change between neighbouring frames' spectra
    FrameDesc key[3];  // at the first, middle and last frame
};

// Smooth a dB curve over +-`oct` octaves (index = harmonic number), averaging
// only the harmonics marked present, so gaps do not read as ripple.
std::vector<double> smoothLog(const std::vector<double> &db, const std::vector<char> &present, int hi, double oct)
{
    std::vector<double> out(db.size(), kFloorDb);
    for (int k = 1; k <= hi; ++k)
    {
        const int lo = std::max(1, int(std::floor(k * std::pow(2.0, -oct))));
        const int up = std::min(hi, int(std::ceil(k * std::pow(2.0, oct))));
        double s = 0;
        int c = 0;
        for (int j = lo; j <= up; ++j)
            if (present[size_t(j)])
                s += db[size_t(j)], ++c;
        out[size_t(k)] = c ? s / c : kFloorDb;
    }
    return out;
}

// Fine ripple: each present harmonic's dB minus the median of its present
// same-parity neighbours (k-4, k-2, k, k+2, k+4). A median is edge-preserving:
// a steep roll-off, a broad formant or a linear slope leave no residual, so
// only per-harmonic detail counts. Working per parity keeps an odd/even
// imbalance (which oddRatio already describes) from reading as ripple.
std::vector<double> rippleOf(const std::vector<double> &db, const std::vector<char> &present, int bw)
{
    std::vector<double> r(db.size(), 0.0);
    for (int k = 1; k <= bw; ++k)
    {
        if (!present[size_t(k)])
            continue;
        // symmetric pairs only: a one-sided window at the ends of the band would
        // put the median off-centre on a slope and invent ripple
        double v[5] = {db[size_t(k)], 0, 0, 0, 0};
        int c = 1;
        for (int dist = 2; dist <= 4; dist += 2)
            if (k - dist >= 1 && k + dist <= bw && present[size_t(k - dist)] && present[size_t(k + dist)])
            {
                v[c++] = db[size_t(k - dist)];
                v[c++] = db[size_t(k + dist)];
            }
        if (c < 3)
            continue;
        std::sort(v, v + c);
        r[size_t(k)] = db[size_t(k)] - v[c / 2];
    }
    return r;
}

// A harmonic is present if it is within 20 dB of the loudest harmonic in its
// +-1/6-octave neighbourhood.
std::vector<char> presence(const std::vector<double> &a, int bw)
{
    std::vector<char> p(a.size(), 0);
    for (int k = 1; k <= bw; ++k)
    {
        const int lo = std::max(1, int(k * 0.891)), up = std::min(bw, int(std::ceil(k * 1.123)));
        double local = 0;
        for (int j = lo; j <= up; ++j)
            local = std::max(local, a[size_t(j)]);
        p[size_t(k)] = a[size_t(k)] >= 0.1 * local && a[size_t(k)] > 0.0;
    }
    return p;
}

FrameDesc describe(const float *frame)
{
    FrameDesc d;
    double sq = 0, pk = 0;
    for (int i = 0; i < kN; ++i)
    {
        sq += double(frame[i]) * frame[i];
        pk = std::max(pk, double(std::fabs(frame[i])));
    }
    d.rms = std::sqrt(sq / kN);
    d.peak = pk;
    d.crest = d.rms > 1e-9 ? pk / d.rms : 1.0;

    const auto a = harmonics(frame);
    double amax = 0, power = 0, powerOdd = 0, powerK = 0;
    for (int k = 1; k <= kMaxH; ++k)
    {
        const double p = a[size_t(k)] * a[size_t(k)];
        amax = std::max(amax, a[size_t(k)]);
        power += p;
        powerK += p * k;
        if (k & 1)
            powerOdd += p;
    }
    if (power <= 1e-18)
        return d;
    d.centroid = powerK / power;
    d.oddRatio = powerOdd / power;

    int bw = 1;
    for (int k = 1; k <= kMaxH; ++k)
        if (a[size_t(k)] >= amax * 1e-3)
            bw = k;
    d.bandwidth = bw;

    // density: harmonics within 20 dB of their +-1/6-octave neighbourhood
    // maximum, counted over odd and even harmonics separately and the larger
    // share kept -- so a square (no evens) is dense, and only a spectrum with
    // gaps in both series (a chord, a bell) is sparse.
    const auto present = presence(a, bw);
    int count[2] = {0, 0}, total[2] = {0, 0};
    for (int k = 1; k <= bw; ++k)
    {
        ++total[k & 1];
        if (present[size_t(k)])
            ++count[k & 1];
    }
    d.density = std::max(total[0] ? double(count[0]) / total[0] : 0.0,
                         total[1] ? double(count[1]) / total[1] : 0.0);

    // slope: fit dB against log2(k), weighting each harmonic by 1/k so every
    // octave counts once, over the harmonics that are present
    std::vector<double> db(size_t(kMaxH) + 1, kFloorDb);
    for (int k = 1; k <= kMaxH; ++k)
        db[size_t(k)] = toDb(a[size_t(k)] / amax);
    double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int k = 1; k <= bw; ++k)
    {
        if (!present[size_t(k)] || db[size_t(k)] < -80.0)
            continue;
        const double w = 1.0 / k, x = std::log2(double(k)), y = db[size_t(k)];
        sw += w;
        sx += w * x;
        sy += w * y;
        sxx += w * x * x;
        sxy += w * x * y;
    }
    const double den = sw * sxx - sx * sx;
    d.slope = den > 1e-12 ? (sw * sxy - sx * sy) / den : 0.0;
    const double icpt = sw > 0 ? (sy - d.slope * sx) / sw : 0.0;

    // formants: bumps of the 1/3-octave-smoothed residual above the fit
    std::vector<double> resid(db.size(), 0.0);
    for (int k = 1; k <= bw; ++k)
        resid[size_t(k)] = std::max(-40.0, db[size_t(k)] - (icpt + d.slope * std::log2(double(k))));
    const auto sm = smoothLog(resid, present, bw, 1.0 / 6.0);
    int p1 = 0;
    for (int k = 1; k <= bw; ++k)
        if (present[size_t(k)] && (!p1 || sm[size_t(k)] > sm[size_t(p1)]))
            p1 = k;
    if (!p1)
        p1 = 1;
    int p2 = 0;
    for (int k = 1; k <= bw; ++k)
        if (present[size_t(k)] && std::fabs(std::log2(double(k) / p1)) >= 1.0 &&
            (!p2 || sm[size_t(k)] > sm[size_t(p2)]))
            p2 = k;
    d.f1Pos = std::log2(double(p1));
    d.f1Gain = std::max(0.0, sm[size_t(p1)]);
    d.f2Pos = p2 ? std::log2(double(p2)) : d.f1Pos;
    d.f2Gain = p2 ? std::max(0.0, sm[size_t(p2)]) : 0.0;

    // roughness: spread of what the smoothing removed, over present harmonics
    const auto rip = rippleOf(db, present, bw);
    double rs = 0;
    int rn = 0;
    for (int k = 1; k <= bw; ++k)
        if (present[size_t(k)] && db[size_t(k)] > -80.0)
        {
            rs += rip[size_t(k)] * rip[size_t(k)];
            ++rn;
        }
    d.roughness = rn ? std::min(12.0, std::sqrt(rs / rn)) : 0.0;
    return d;
}

TableDesc describe(const Table &t)
{
    TableDesc d;
    d.frames = int(t.frames.size());
    const size_t n = t.frames.size();
    d.key[0] = describe(t.frames[0].data());
    d.key[1] = describe(t.frames[(n - 1) / 2].data());
    d.key[2] = describe(t.frames[n - 1].data());
    if (n > 1)
    {
        double tot = 0;
        std::vector<double> prev;
        for (size_t f = 0; f < n; ++f)
        {
            const auto a = harmonics(t.frames[f].data());
            std::vector<double> db(129);
            for (int k = 1; k <= 128; ++k)
                db[size_t(k)] = std::max(-80.0, toDb(a[size_t(k)]));
            if (!prev.empty())
            {
                double s = 0;
                for (int k = 1; k <= 128; ++k)
                    s += (db[size_t(k)] - prev[size_t(k)]) * (db[size_t(k)] - prev[size_t(k)]);
                tot += std::sqrt(s / 128.0);
            }
            prev = db;
        }
        d.motion = tot / double(n - 1);
    }
    return d;
}

// --- descriptor JSON (flat, so any tool or model can read and write it) --------

const char *kFields[] = {"rms",   "peak",   "centroid", "slope",     "oddRatio", "bandwidth", "f1Pos",
                         "f1Gain", "f2Pos", "f2Gain",   "roughness", "density",  "crest"};
const char *kKeys[] = {"start", "mid", "end"};

double *field(FrameDesc &f, int i)
{
    double *p[] = {&f.rms,   &f.peak,   &f.centroid, &f.slope,     &f.oddRatio, &f.bandwidth, &f.f1Pos,
                   &f.f1Gain, &f.f2Pos, &f.f2Gain,   &f.roughness, &f.density,  &f.crest};
    return p[i];
}
double field(const FrameDesc &f, int i) { return *field(const_cast<FrameDesc &>(f), i); }
constexpr int kFieldCount = int(sizeof(kFields) / sizeof(kFields[0]));

std::string toJson(const TableDesc &d)
{
    std::ostringstream o;
    o.precision(6);
    o << "{\n  \"frames\": " << d.frames << ",\n  \"motion\": " << d.motion;
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < kFieldCount; ++i)
            o << ",\n  \"" << kKeys[k] << '.' << kFields[i] << "\": " << field(d.key[k], i);
    o << "\n}\n";
    return o.str();
}

bool fromJson(const std::string &s, TableDesc &d, std::string &err)
{
    std::map<std::string, double> kv;
    for (size_t pos = 0; (pos = s.find('"', pos)) != std::string::npos;)
    {
        const size_t end = s.find('"', pos + 1);
        const size_t colon = end == std::string::npos ? end : s.find(':', end);
        if (colon == std::string::npos)
            break;
        kv[s.substr(pos + 1, end - pos - 1)] = std::strtod(s.c_str() + colon + 1, nullptr);
        pos = s.find_first_of(",}", colon);
        if (pos == std::string::npos)
            break;
    }
    if (!kv.count("frames"))
        return err = "descriptor has no \"frames\"", false;
    d.frames = std::clamp(int(kv["frames"]), 1, 256);
    if (kv.count("motion"))
        d.motion = kv["motion"];
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < kFieldCount; ++i)
        {
            auto it = kv.find(std::string(kKeys[k]) + "." + kFields[i]);
            if (it != kv.end())
                *field(d.key[k], i) = it->second;
        }
    return true;
}

// --- the generator: descriptor + seed in, table out. No reference samples. -------

FrameDesc lerp(const FrameDesc &a, const FrameDesc &b, double t)
{
    FrameDesc r = a;
    for (int i = 0; i < kFieldCount; ++i)
        *field(r, i) = field(a, i) * (1 - t) + field(b, i) * t;
    return r;
}

double centroidOf(const std::vector<double> &a)
{
    double p = 0, pk = 0;
    for (int k = 1; k <= kMaxH; ++k)
    {
        p += a[size_t(k)] * a[size_t(k)];
        pk += a[size_t(k)] * a[size_t(k)] * k;
    }
    return p > 0 ? pk / p : 1.0;
}

int bandwidthOf(const std::vector<double> &a)
{
    double amax = 0;
    for (int k = 1; k <= kMaxH; ++k)
        amax = std::max(amax, a[size_t(k)]);
    int bw = 1;
    for (int k = 1; k <= kMaxH; ++k)
        if (a[size_t(k)] >= amax * 1e-3)
            bw = k;
    return bw;
}

Table generate(const TableDesc &d, uint64_t seed)
{
    Rng rng(seed);
    const int n = std::max(1, d.frames);

    // Per-table random fields: fine ripple (two, crossfaded across the table so
    // the ripple moves), phases, a presence lottery ticket per harmonic, and one
    // chord (our own ratio sets) for frames that are sparse enough to be chordal.
    std::vector<double> ripA(kMaxH + 1), ripB(kMaxH + 1), phase(kMaxH + 1), ticket(kMaxH + 1);
    for (int k = 1; k <= kMaxH; ++k)
    {
        ripA[size_t(k)] = rng.range(-1.7, 1.7); // std ~1
        ripB[size_t(k)] = rng.range(-1.7, 1.7);
        phase[size_t(k)] = rng.range(-kPi, kPi);
        ticket[size_t(k)] = rng.uni();
    }
    // Only chords whose root sits well under the darkest chordal keypoint can
    // reach its brightness: a chord on harmonic 10 cannot sound darker than 10.
    // With none that fits, sparse frames thin at random instead (keeping 1).
    static const std::vector<std::vector<int>> chords = {
        {4, 5, 6}, {10, 12, 15}, {4, 5, 6, 7}, {8, 10, 12, 15}, {2, 3}, {3, 4, 5}, {6, 8, 9}};
    // The parity matters too: an odd-heavy target needs a low ODD chord member,
    // or its odd power can only sit high up and drag the centroid with it.
    double darkest = 1e9, oddShare = 0.5;
    for (const auto &kd : d.key)
        if (kd.density < 0.5 && kd.centroid < darkest)
            darkest = kd.centroid, oddShare = kd.oddRatio;
    auto lowest = [](const std::vector<int> &c, bool odd) {
        int best = 1 << 20;
        for (int r : c)
            best = std::min(best, odd ? ((r & 1) ? r : 1 << 20) : ((r & 1) ? 2 * r : r));
        return best;
    };
    std::vector<const std::vector<int> *> fits;
    for (const auto &c : chords)
        if (c[0] <= 0.6 * darkest && lowest(c, oddShare >= 0.5) <= 0.6 * darkest)
            fits.push_back(&c);
    const std::vector<int> *chord = fits.empty() ? nullptr : fits[size_t(rng.next() % fits.size())];
    std::vector<char> inChord(kMaxH + 1, 0);
    if (chord)
        for (int k = 1; k <= kMaxH; ++k)
            for (int r : *chord)
                if (k % r == 0)
                    inChord[size_t(k)] = 1;

    // Which harmonics a frame keeps. Dense frames keep all. Sparser frames keep
    // the harmonics whose ticket is under a threshold, so as density moves
    // across the table harmonics drop out and return consistently rather than
    // reshuffling. Frames under 0.5 draw only from the chord's series.
    auto keepFor = [&](const FrameDesc &fd) {
        std::vector<double> keep(kMaxH + 1, 1.0);
        if (fd.density >= 0.9)
            return keep;
        const bool chordal = chord && fd.density < 0.5;
        const int bw = std::clamp(int(fd.bandwidth), 1, kMaxH);
        int inSet = 0;
        for (int k = 1; k <= bw; ++k)
            if (!chordal || inChord[size_t(k)])
                ++inSet;
        const double thin = std::min(1.0, fd.density * bw / std::max(1, inSet));
        for (int k = 1; k <= kMaxH; ++k)
            keep[size_t(k)] = (!chordal || inChord[size_t(k)]) && ticket[size_t(k)] < thin ? 1.0 : 0.0;
        keep[size_t(chordal ? (*chord)[0] : 1)] = 1.0;
        if (chordal) // the lowest odd and even members survive thinning
        {
            const int lo = lowest(*chord, true), le = lowest(*chord, false);
            if (lo <= kMaxH)
                keep[size_t(lo)] = 1.0;
            if (le <= kMaxH)
                keep[size_t(le)] = 1.0;
        }
        else
            keep[2] = 1.0;
        return keep;
    };

    // Per-frame targets move through the three keypoints, plus a seeded wobble
    // on the frames between them that scales with the reference's motion.
    const double tMid = n > 1 ? double((n - 1) / 2) / double(n - 1) : 0.0;
    std::vector<FrameDesc> target(static_cast<size_t>(n));
    for (int f = 0; f < n; ++f)
    {
        const double t = n > 1 ? double(f) / double(n - 1) : 0.0;
        FrameDesc fd;
        if (n == 1)
            fd = d.key[0];
        else if (tMid > 0.0 && t <= tMid)
            fd = lerp(d.key[0], d.key[1], t / tMid);
        else // tMid < 1 whenever n > 1
            fd = lerp(d.key[1], d.key[2], (t - tMid) / (1.0 - tMid));
        if (n > 1 && f != 0 && f != (n - 1) / 2 && f != n - 1) // keypoint frames stay exact
        {
            const double wob = std::min(1.0, d.motion / 12.0);
            fd.centroid *= std::pow(2.0, rng.range(-0.35, 0.35) * wob);
            fd.f1Pos += rng.range(-0.5, 0.5) * wob;
            fd.f2Pos += rng.range(-0.5, 0.5) * wob;
        }
        target[size_t(f)] = fd;
    }

    auto spectrum = [&](const FrameDesc &fd, const std::vector<double> &keep, double t, double cutoff,
                        double tilt, double oddW) {
        std::vector<double> a(kMaxH + 1, 0.0);
        for (int k = 1; k <= kMaxH; ++k)
        {
            const double lk = std::log2(double(k));
            double db = (fd.slope + tilt) * lk;
            db += fd.f1Gain * std::exp(-0.5 * std::pow((lk - fd.f1Pos) / 0.35, 2.0));
            db += fd.f2Gain * std::exp(-0.5 * std::pow((lk - fd.f2Pos) / 0.35, 2.0));
            db += fd.roughness * (ripA[size_t(k)] * (1 - t) + ripB[size_t(k)] * t);
            double v = std::pow(10.0, db / 20.0) * keep[size_t(k)];
            v /= std::sqrt(1.0 + std::pow(double(k) / cutoff, 16.0));
            if (k & 1)
                v *= oddW;
            a[size_t(k)] = v;
        }
        return a;
    };

    auto oddWeightFor = [&](const std::vector<double> &a, double ratio) {
        double po = 0, pe = 0;
        for (int k = 1; k <= kMaxH; ++k)
            (k & 1 ? po : pe) += a[size_t(k)] * a[size_t(k)];
        ratio = std::clamp(ratio, 0.02, 0.999);
        if (po <= 0 || pe <= 0)
            return 1.0;
        return std::sqrt(ratio / (1.0 - ratio) * pe / po);
    };

    Table out;
    out.frames.resize(size_t(n));
    std::vector<std::vector<double>> amps(static_cast<size_t>(n));
    for (int f = 0; f < n; ++f)
    {
        const FrameDesc &fd = target[size_t(f)];
        const auto keep = keepFor(fd);
        const double t = n > 1 ? double(f) / double(n - 1) : 0.0;
        double cutoff = 4096.0, tilt = 0.0, oddW = 1.0;
        // Each step disturbs the others slightly (the odd weight moves the
        // centroid, the tilt moves the bandwidth), so iterate to a fixed point.
        for (int round = 0; round < 6; ++round)
        {
            // bandwidth: lower the cutoff until the -60 dB edge sits at the target
            cutoff = 4096.0;
            if (bandwidthOf(spectrum(fd, keep, t, cutoff, tilt, oddW)) > fd.bandwidth)
            {
                double lo = 0.0, hi = 12.0; // log2(cutoff)
                for (int it = 0; it < 30; ++it)
                {
                    const double mid = 0.5 * (lo + hi);
                    (bandwidthOf(spectrum(fd, keep, t, std::pow(2.0, mid), tilt, oddW)) > fd.bandwidth ? hi : lo) =
                        mid;
                }
                cutoff = std::pow(2.0, 0.5 * (lo + hi));
            }
            // brightness: an extra tilt until the centroid matches
            double lo = -24.0, hi = 24.0;
            for (int it = 0; it < 40; ++it)
            {
                const double mid = 0.5 * (lo + hi);
                (centroidOf(spectrum(fd, keep, t, cutoff, mid, oddW)) > fd.centroid ? hi : lo) = mid;
            }
            tilt = 0.5 * (lo + hi);
            // hollowness: odd/even balance, in closed form
            oddW *= oddWeightFor(spectrum(fd, keep, t, cutoff, tilt, oddW), fd.oddRatio);
        }
        amps[size_t(f)] = spectrum(fd, keep, t, cutoff, tilt, oddW);
    }

    // Phases: one blend of aligned (0) and random for the whole table, chosen
    // so the three keypoint frames' crest factors land nearest their targets.
    const size_t keyFrame[3] = {0, size_t((n - 1) / 2), size_t(n - 1)};
    double bestBeta = 0, bestErr = 1e9;
    for (int b = 0; b <= 20; ++b)
    {
        const double beta = b / 20.0;
        std::vector<double> ph(kMaxH + 1);
        for (int k = 1; k <= kMaxH; ++k)
            ph[size_t(k)] = beta * phase[size_t(k)];
        double e = 0;
        for (int key = 0; key < 3; ++key)
        {
            const auto x = synthesize(amps[keyFrame[key]], ph);
            double sq = 0, pk = 0;
            for (float v : x)
            {
                sq += double(v) * v;
                pk = std::max(pk, double(std::fabs(v)));
            }
            e += std::fabs((sq > 0 ? pk / std::sqrt(sq / kN) : 1.0) - d.key[key].crest);
        }
        if (e < bestErr)
            bestErr = e, bestBeta = beta;
    }
    std::vector<double> ph(kMaxH + 1);
    for (int k = 1; k <= kMaxH; ++k)
        ph[size_t(k)] = bestBeta * phase[size_t(k)];

    // Level: each frame to its target RMS, then the whole table to the target peak.
    double tablePeak = 0;
    for (int f = 0; f < n; ++f)
    {
        auto x = synthesize(amps[size_t(f)], ph);
        double sq = 0;
        for (float v : x)
            sq += double(v) * v;
        const double r = std::sqrt(sq / kN);
        const double g = r > 0 ? target[size_t(f)].rms / r : 0.0;
        for (float &v : x)
        {
            v = float(v * g);
            tablePeak = std::max(tablePeak, double(std::fabs(v)));
        }
        out.frames[size_t(f)] = std::move(x);
    }
    const double wantPeak = std::clamp(std::max({d.key[0].peak, d.key[1].peak, d.key[2].peak}), 0.05, 1.0);
    if (tablePeak > 0)
        for (auto &fr : out.frames)
            for (float &v : fr)
                v = float(v * wantPeak / tablePeak);
    return out;
}

// How far a table's own descriptors sit from a target: the worst keypoint's
// log brightness error, or twice its odd-ratio error, whichever is larger.
double descError(const TableDesc &want, const TableDesc &got)
{
    double e = got.frames == want.frames ? 0.0 : 1e6;
    for (int k = 0; k < 3; ++k)
    {
        e = std::max(e, std::fabs(std::log(std::max(got.key[k].centroid, 1e-6) /
                                           std::max(want.key[k].centroid, 1e-6))));
        e = std::max(e, 2.0 * std::fabs(got.key[k].oddRatio - want.key[k].oddRatio));
    }
    return e;
}

// Some seeds draw a mask or chord that cannot meet every target at once. Try
// a few and keep the candidate whose OWN descriptors land nearest the numbers.
// This compares output with the descriptor only, never with a reference; it is
// the same "roll again" a user gets from a generate button.
Table generateBest(const TableDesc &d, uint64_t seed, int tries)
{
    Table best;
    double bestErr = 1e18;
    for (int i = 0; i < std::max(1, tries); ++i)
    {
        Table t = generate(d, seed + 0x9E3779B97F4A7C15ull * uint64_t(i));
        const double e = descError(d, describe(t));
        if (e < bestErr)
        {
            bestErr = e;
            best = std::move(t);
        }
        if (bestErr < 0.05) // within ~5% brightness and 0.025 odd ratio: done
            break;
    }
    return best;
}

// A descriptor drawn from broad priors: the "random" button.
TableDesc randomDesc(uint64_t seed, int frames)
{
    Rng r(seed ^ 0xA5A5A5A5ull);
    TableDesc d;
    d.frames = frames;
    d.motion = r.range(0.5, 10.0);
    for (auto &k : d.key)
    {
        k.slope = r.range(-12.0, -2.0);
        k.bandwidth = std::pow(2.0, r.range(3.0, 10.0));
        k.centroid = std::pow(2.0, r.range(0.3, std::log2(k.bandwidth) * 0.7));
        k.oddRatio = r.range(0.35, 0.98);
        // A centroid near 1 means nearly all power in the (odd) fundamental, so
        // it cannot coexist with a low odd ratio. Keep the pair satisfiable.
        k.centroid = std::max(k.centroid, 1.0 + 2.0 * (1.0 - k.oddRatio));
        k.f1Pos = r.range(1.0, 7.0);
        k.f1Gain = r.uni() < 0.5 ? r.range(0.0, 18.0) : 0.0;
        k.f2Pos = k.f1Pos + r.range(1.0, 3.0);
        k.f2Gain = r.uni() < 0.3 ? r.range(0.0, 12.0) : 0.0;
        k.roughness = r.range(0.0, 4.0);
        k.density = r.uni() < 0.8 ? 1.0 : r.range(0.2, 0.8);
        k.crest = r.range(1.3, 3.0);
        k.rms = 0.3;
        k.peak = 0.95;
    }
    return d;
}

// --- verification: the only place a reference and its counterpart meet ---------

// Max over circular shifts of normalised cross-correlation. 1.0 = same waveform.
double maxXcorr(const std::vector<float> &x, const std::vector<float> &y)
{
    std::vector<cd> a(kN), b(kN);
    double mx = 0, my = 0;
    for (int i = 0; i < kN; ++i)
        mx += x[size_t(i)], my += y[size_t(i)];
    mx /= kN, my /= kN;
    double ex = 0, ey = 0;
    for (int i = 0; i < kN; ++i)
    {
        a[size_t(i)] = x[size_t(i)] - mx;
        b[size_t(i)] = y[size_t(i)] - my;
        ex += std::norm(a[size_t(i)]);
        ey += std::norm(b[size_t(i)]);
    }
    if (ex <= 0 || ey <= 0)
        return 0.0;
    fft(a, false);
    fft(b, false);
    for (int i = 0; i < kN; ++i)
        a[size_t(i)] *= std::conj(b[size_t(i)]);
    fft(a, true);
    double best = 0;
    for (const auto &v : a)
        best = std::max(best, std::fabs(v.real()));
    return best / std::sqrt(ex * ey);
}

// Correlation of the fine spectral ripple (what 1/3-octave smoothing removes).
// A copy reproduces it (~1). An independent table does not (~0). Returns NaN
// when the reference has no ripple to copy (a clean saw, square or sine).
double rippleCorr(const std::vector<float> &x, const std::vector<float> &y, int *harmonicsUsed = nullptr)
{
    auto ripple = [](const std::vector<float> &v, int &bw, std::vector<char> &present) {
        const auto a = harmonics(v.data());
        double amax = 0;
        for (int k = 1; k <= kMaxH; ++k)
            amax = std::max(amax, a[size_t(k)]);
        bw = std::min(256, bandwidthOf(a));
        present = presence(a, bw);
        std::vector<double> db(size_t(kMaxH) + 1, kFloorDb);
        for (int k = 1; k <= kMaxH; ++k)
            db[size_t(k)] = std::max(-80.0, toDb(a[size_t(k)] / std::max(amax, 1e-12)));
        return rippleOf(db, present, bw);
    };
    int bx = 0, by = 0;
    std::vector<char> px, py;
    const auto rx = ripple(x, bx, px), ry = ripple(y, by, py);
    const int hi = std::min(bx, by);
    double sx = 0, sy = 0, sxy = 0, sxx = 0, syy = 0;
    int m = 0;
    for (int k = 1; k <= hi; ++k)
    {
        if (!px[size_t(k)] || !py[size_t(k)])
            continue;
        ++m;
        sx += rx[size_t(k)], sy += ry[size_t(k)];
        sxy += rx[size_t(k)] * ry[size_t(k)];
        sxx += rx[size_t(k)] * rx[size_t(k)];
        syy += ry[size_t(k)] * ry[size_t(k)];
    }
    if (harmonicsUsed)
        *harmonicsUsed = m;
    // Too few shared harmonics and the test has no power: with m harmonics an
    // independent correlation has a spread of about 1/sqrt(m), and a pack makes
    // thousands of comparisons. At m = 31 that spread is 0.18, and chance alone
    // reaches ~0.7 somewhere (seen). 48 halves the false alarms, and a frame with
    // fewer present harmonics is too simple to hold anything worth copying.
    if (m < 48)
        return std::nan("");
    const double vx = sxx - sx * sx / m, vy = syy - sy * sy / m;
    if (vy / m < 0.25) // reference ripple under 0.5 dB rms: nothing to copy
        return std::nan("");
    return vx > 0 && vy > 0 ? (sxy - sx * sy / m) / std::sqrt(vx * vy) : 0.0;
}

// --- commands ----------------------------------------------------------------------

uint64_t seedArg(int argc, char **argv, uint64_t dflt)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--seed"))
            return std::strtoull(argv[i + 1], nullptr, 10);
    return dflt;
}

const char *strArg(int argc, char **argv, const char *flag)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], flag))
            return argv[i + 1];
    return nullptr;
}

int cmdPack(const fs::path &refDir, const fs::path &outDir, uint64_t seed, const char *reportPath)
{
    std::vector<fs::path> files;
    for (const auto &e : fs::recursive_directory_iterator(refDir))
        if (e.is_regular_file())
        {
            auto ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".wav")
                files.push_back(e.path());
        }
    std::sort(files.begin(), files.end());

    std::ostringstream rep, descs;
    rep << "file\tframes\tcentroid_err%\toddRatio_err\tbandwidth_err%\tslope_err_dB/oct\tcrest_err"
           "\tdeepest_null_dB\txcorr_rich\tripple_corr\tripple_null\tsimilar\tcopy_check\n";
    int ok = 0, similar = 0, failed = 0, flagged = 0;
    double worstRipple = -2, worstNull = -2, worstXc = -1, deepestNull = 0;
    descs << "{\n";
    auto fmt3 = [](double v) { return v < -1 ? std::string("n/a") : std::to_string(v).substr(0, v < 0 ? 6 : 5); };
    for (const auto &path : files)
    {
        const auto rel = fs::relative(path, refDir);
        Table ref;
        std::string err;
        if (!readTable(path, ref, err))
        {
            std::printf("  SKIP %s: %s\n", rel.string().c_str(), err.c_str());
            ++failed;
            continue;
        }

        // 1. reference -> descriptor. The reference samples are dropped here.
        TableDesc desc = describe(ref);
        const double rate = ref.sampleRate;
        ref = Table{};

        // 2. descriptor + seed -> new table. Nothing else goes in.
        const uint64_t fileSeed = hashString(rel.generic_string(), seed);
        Table gen = generateBest(desc, fileSeed, 4);
        gen.sampleRate = rate;
        const auto outPath = outDir / rel;
        fs::create_directories(outPath.parent_path());
        if (!writeTable(outPath, gen))
        {
            std::printf("  FAIL could not write %s\n", outPath.string().c_str());
            ++failed;
            continue;
        }

        // 3. verify: re-read both from disk and compare.
        Table back, again;
        readTable(outPath, back, err);
        readTable(path, again, err);
        const TableDesc got = describe(back);
        double cErr = 0, oErr = 0, bErr = 0, sErr = 0, crErr = 0;
        for (int k = 0; k < 3; ++k)
        {
            const auto &w = desc.key[k], &g = got.key[k];
            cErr = std::max(cErr, std::fabs(g.centroid / w.centroid - 1.0));
            oErr = std::max(oErr, std::fabs(g.oddRatio - w.oddRatio));
            bErr = std::max(bErr, std::fabs(g.bandwidth / w.bandwidth - 1.0));
            sErr = std::max(sErr, std::fabs(g.slope - w.slope));
            crErr = std::max(crErr, std::fabs(g.crest - w.crest));
        }

        // Not a copy? ADR-0010's test comes first: the deepest best-case null of
        // any generated frame against any reference frame (best gain, circular
        // shift and polarity; 10 log10(1 - xcorr^2)). Identical audio nulls to
        // -90 dB and below. Two exemptions, because the shape is shared, not
        // the data: a pure sine nulls against any pure sine (-55 dB seen, limited
        // only by whole-sample alignment), and a band-limited square, saw or
        // triangle reaches about -30 dB against any other of its kind.
        // Waveform correlation is then reported only against reference frames
        // with content to copy. Ripple correlation needs 48+ shared harmonics
        // (rippleCorr returns NaN otherwise). The ripple null is the same
        // descriptor with a different seed: how much ripple two tables share
        // when the descriptor is ALL they share. A copy would sit far above it.
        std::vector<char> rich(again.frames.size());
        for (size_t r = 0; r < again.frames.size(); ++r)
        {
            const FrameDesc fd = describe(again.frames[r].data());
            rich[r] = fd.centroid >= 2.0 && fd.bandwidth >= 24.0;
        }
        const Table alt = generateBest(desc, fileSeed ^ 0x5EEDull, 4);
        double xc = -2, rc = -2, rn = -2, xcAll = 0;
        for (size_t f = 0; f < back.frames.size(); ++f)
        {
            for (size_t r = 0; r < again.frames.size(); ++r)
            {
                const double x = maxXcorr(back.frames[f], again.frames[r]);
                xcAll = std::max(xcAll, x);
                if (rich[r])
                    xc = std::max(xc, x);
                const double rr = rippleCorr(back.frames[f], again.frames[r]);
                if (!std::isnan(rr))
                    rc = std::max(rc, rr);
            }
            // same number of comparisons as against the reference, or the max
            // over more pairs would look worse by chance alone
            for (size_t r = 0; r < alt.frames.size(); ++r)
            {
                const double nn = rippleCorr(back.frames[f], alt.frames[r]);
                if (!std::isnan(nn))
                    rn = std::max(rn, nn);
            }
        }
        const double nullDb = 10.0 * std::log10(std::max(1e-15, 1.0 - xcAll * xcAll));
        const bool sim = got.frames == desc.frames && cErr < 0.15 && oErr < 0.1;
        const bool check = rc > 0.6 && rc > rn + 0.3;
        if (sim)
            ++similar;
        if (check)
            ++flagged;
        worstRipple = std::max(worstRipple, rc);
        worstNull = std::max(worstNull, rn);
        worstXc = std::max(worstXc, xc);
        deepestNull = std::min(deepestNull, nullDb);
        descs << (ok ? ",\n" : "") << "\"" << rel.generic_string() << "\": " << toJson(desc);
        ++ok;
        char line[512];
        std::snprintf(line, sizeof(line), "%s\t%d\t%.1f\t%.3f\t%.1f\t%.2f\t%.2f\t%.1f\t%s\t%s\t%s\t%s\t%s\n",
                      rel.generic_string().c_str(), got.frames, cErr * 100, oErr, bErr * 100, sErr, crErr, nullDb,
                      fmt3(xc).c_str(), fmt3(rc).c_str(), fmt3(rn).c_str(), sim ? "yes" : "no",
                      rc < -1 ? "n/a" : (check ? "CHECK" : "ok"));
        rep << line;
        std::printf("  %-44s %2d fr  centroid %4.1f%%  odd %.3f  null %6.1f dB  ripple %-6s (null %-6s) %s%s\n",
                    rel.generic_string().c_str(), got.frames, cErr * 100, oErr, nullDb, fmt3(rc).c_str(),
                    fmt3(rn).c_str(), sim ? "similar" : "DIFFERENT", check ? "  COPY-CHECK" : "");
    }
    descs << "\n}\n";
    std::printf("wtgen pack: %d written, %d similar by descriptor, %d failed\n", ok, similar, failed);
    std::printf("            not-a-copy: deepest best-case null %.1f dB; max ripple vs reference %s (null %s), "
                "max xcorr on rich frames %s, %d flagged\n",
                deepestNull, fmt3(worstRipple).c_str(), fmt3(worstNull).c_str(), fmt3(worstXc).c_str(), flagged);
    if (reportPath)
    {
        std::ofstream(reportPath) << rep.str();
        std::ofstream(std::string(reportPath) + ".descriptors.json") << descs.str();
    }
    return failed == 0 ? 0 : 1;
}

int cmdSelftest()
{
    int fails = 0;
    auto expect = [&](bool ok, const char *what, double got) {
        std::printf("  %-4s %-46s %.4f\n", ok ? "ok" : "FAIL", what, got);
        if (!ok)
            ++fails;
    };
    auto shape = [](auto fn) {
        std::vector<float> x(kN);
        for (int i = 0; i < kN; ++i)
            x[size_t(i)] = float(fn(double(i) / kN));
        return x;
    };
    // band-limited saw and square by additive synthesis, so the truth is exact
    auto additive = [](bool oddOnly, int top) {
        std::vector<double> a(kMaxH + 1, 0.0), ph(kMaxH + 1, 0.0);
        for (int k = 1; k <= top; ++k)
            if (!oddOnly || (k & 1))
                a[size_t(k)] = 1.0 / k;
        return synthesize(a, ph);
    };
    const auto saw = additive(false, kMaxH), sq = additive(true, kMaxH);
    const auto sine = shape([](double t) { return std::sin(2 * kPi * t); });
    const auto s = describe(saw.data()), q = describe(sq.data()), n = describe(sine.data());
    expect(std::fabs(s.slope + 6.02) < 0.1, "saw slope is -6.02 dB/oct", s.slope);
    expect(std::fabs(s.oddRatio - 0.75) < 0.01, "saw odd ratio is 0.75 (pi^2/8 / pi^2/6)", s.oddRatio);
    expect(std::fabs(q.oddRatio - 1.0) < 1e-6, "square odd ratio is 1", q.oddRatio);
    expect(std::fabs(q.slope + 6.02) < 0.3, "square slope is -6 dB/oct", q.slope);
    expect(std::fabs(n.centroid - 1.0) < 1e-6, "sine centroid is harmonic 1", n.centroid);
    expect(n.bandwidth == 1, "sine bandwidth is 1", n.bandwidth);
    expect(s.density > 0.99, "saw density is 1", s.density);
    expect(q.density > 0.99, "square density is 1 (no evens is not sparse)", q.density);
    expect(s.roughness < 0.1, "saw has no ripple", s.roughness);

    // Round trip on a consistent synthetic "reference": 8 frames whose slope
    // falls, whose formant climbs, whose even harmonics fade, with 3 dB of
    // seeded ripple. Describe it, generate from the description, describe that.
    Table ref;
    Rng rr(99);
    std::vector<double> ripple(kMaxH + 1);
    for (auto &v : ripple)
        v = rr.range(-5.2, 5.2); // std 3 dB
    for (int f = 0; f < 8; ++f)
    {
        std::vector<double> a(kMaxH + 1, 0.0), ph(kMaxH + 1, 0.0);
        for (int k = 1; k <= 200 + 50 * f; ++k)
        {
            const double lk = std::log2(double(k));
            double db = (-3.0 - f) * lk + 12.0 * std::exp(-0.5 * std::pow((lk - 3.0 - 0.4 * f) / 0.4, 2.0));
            db += ripple[size_t(k)] + ((k % 2 == 0 && f >= 4) ? -12.0 : 0.0);
            a[size_t(k)] = std::pow(10.0, db / 20.0);
        }
        ref.frames.push_back(synthesize(a, ph));
    }
    const TableDesc want = describe(ref);
    const Table g = generate(want, 7);
    const TableDesc back = describe(g);
    double worstC = 0, worstO = 0;
    for (int k = 0; k < 3; ++k)
    {
        worstC = std::max(worstC, std::fabs(back.key[k].centroid / want.key[k].centroid - 1.0));
        worstO = std::max(worstO, std::fabs(back.key[k].oddRatio - want.key[k].oddRatio));
    }
    expect(back.frames == 8, "generated table has 8 frames", back.frames);
    expect(worstC < 0.15, "round trip: centroid within 15% at every key", worstC);
    expect(worstO < 0.10, "round trip: odd ratio within 0.10 at every key", worstO);
    double worstRipple = -1;
    for (int f = 0; f < 8; ++f)
        worstRipple = std::max(worstRipple, rippleCorr(g.frames[size_t(f)], ref.frames[size_t(f)]));
    expect(worstRipple < 0.5, "round trip: reference ripple NOT reproduced", worstRipple);
    expect(rippleCorr(ref.frames[3], ref.frames[3]) > 0.99, "ripple check detects a real copy", 1.0);
    const Table g2 = generate(want, 7);
    expect(g2.frames[3] == g.frames[3], "same seed, same table", 1.0);
    std::printf("wtgen selftest: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "selftest")
        return cmdSelftest();
    if (cmd == "analyze" && argc >= 3)
    {
        Table t;
        std::string err;
        if (!readTable(fs::u8path(argv[2]), t, err))
            return std::fprintf(stderr, "wtgen: %s\n", err.c_str()), 2;
        std::fputs(toJson(describe(t)).c_str(), stdout);
        return 0;
    }
    if (cmd == "generate" && argc >= 4)
    {
        std::ifstream f(fs::u8path(argv[2]));
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), err;
        TableDesc d;
        if (!fromJson(s, d, err))
            return std::fprintf(stderr, "wtgen: %s\n", err.c_str()), 2;
        return writeTable(fs::u8path(argv[3]), generateBest(d, seedArg(argc, argv, 1), 4)) ? 0 : 2;
    }
    if (cmd == "random" && argc >= 3)
    {
        const char *fr = strArg(argc, argv, "--frames");
        const uint64_t seed = seedArg(argc, argv, 1);
        const TableDesc d = randomDesc(seed, fr ? std::clamp(std::atoi(fr), 1, 256) : 16);
        std::fputs(toJson(d).c_str(), stdout); // the dice roll, reusable with `generate`
        return writeTable(fs::u8path(argv[2]), generateBest(d, seed, 4)) ? 0 : 2;
    }
    if (cmd == "compare" && argc >= 4) // every frame of A against every frame of B
    {
        Table a, b;
        std::string err;
        if (!readTable(fs::u8path(argv[2]), a, err) || !readTable(fs::u8path(argv[3]), b, err))
            return std::fprintf(stderr, "wtgen: %s\n", err.c_str()), 2;
        // null_dB: what is left after the best gain, circular shift and polarity,
        // relative to B's energy -- 10 log10(1 - xcorr^2). A phase-inverted A/B
        // in a DAW, with no alignment, can only null less deeply than this.
        std::printf("A\tB\txcorr\tnull_dB\tripple\tharmonics\n");
        for (size_t f = 0; f < a.frames.size(); ++f)
            for (size_t r = 0; r < b.frames.size(); ++r)
            {
                int m = 0;
                const double rr = rippleCorr(a.frames[f], b.frames[r], &m);
                const double xc = maxXcorr(a.frames[f], b.frames[r]);
                std::printf("%zu\t%zu\t%.9f\t%.1f\t%s\t%d\n", f, r, xc,
                            10.0 * std::log10(std::max(1e-15, 1.0 - xc * xc)),
                            std::isnan(rr) ? "n/a" : std::to_string(rr).substr(0, 6).c_str(), m);
            }
        return 0;
    }
    if (cmd == "pack" && argc >= 4)
        return cmdPack(fs::u8path(argv[2]), fs::u8path(argv[3]), seedArg(argc, argv, 1),
                       strArg(argc, argv, "--report"));
    std::fprintf(stderr, "usage: wtgen analyze|generate|random|pack|selftest ... (see the header of wtgen.cpp)\n");
    return 2;
}
