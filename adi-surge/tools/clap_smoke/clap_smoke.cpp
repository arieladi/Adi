// clap_smoke -- load a .clap with no DAW, play one note, prove it made sound.
//
//   clap_smoke <plugin.clap> [--id <plugin-id>] [--wav <out.wav>]
//
// Onboarding step 4.4 asks for the built CLAP to be loaded in a host and heard.
// This is that host, headless: it loads the library, creates the plugin through
// the CLAP C ABI, plays A4 (key 69) for one second, releases it, and then checks
// what came out instead of trusting that something did:
//
//   - silence before the note             peak below -80 dBFS
//   - sound during it, on both channels   RMS above -40 dBFS
//   - the right note                      440 Hz within 1%, by YIN pitch estimate
//   - no NaN or Inf anywhere
//   - silence again after the release     tail at least 40 dB below the note
//
// Exit 0 when every check passes, 1 when any fails, 2 when the plugin cannot be
// created at all. Built against the CLAP headers Surge already vendors
// (libs/clap-juce-extensions/clap-libs/clap, MIT), so it adds no dependency and
// shares no code with any other project in the monorepo.
//
// Threads are used the way a DAW uses them: the main thread creates, activates
// and destroys the plugin and services on_main_thread() requests, and a second
// thread runs process(). On Windows the main thread also pumps the message
// queue, because a JUCE-based plugin posts work to it and a console program has
// no message loop of its own.

#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kBlock = 512;
constexpr int16_t kKey = 69; // A4
constexpr double kExpectedHz = 440.0;
constexpr double kPreSeconds = 0.25;
constexpr double kHoldSeconds = 1.0;
constexpr double kTailSeconds = 2.0;

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char *what, const std::string &detail)
{
    ++g_checks;
    if (!ok)
        ++g_failures;
    std::printf("  %-4s %-40s %s\n", ok ? "ok" : "FAIL", what, detail.c_str());
}

double dbfs(double linear) { return linear > 0.0 ? 20.0 * std::log10(linear) : -999.0; }

std::string fmt(const char *format, double value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), format, value);
    return buf;
}

// --- the shared library --------------------------------------------------

struct Library
{
#ifdef _WIN32
    HMODULE handle = nullptr;
    bool open(const char *path)
    {
        handle = LoadLibraryW(std::filesystem::u8path(path).wstring().c_str());
        return handle != nullptr;
    }
    const void *symbol(const char *name) const
    {
        return reinterpret_cast<const void *>(GetProcAddress(handle, name));
    }
    void close()
    {
        if (handle)
            FreeLibrary(handle);
        handle = nullptr;
    }
#else
    void *handle = nullptr;
    bool open(const char *path)
    {
        handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        return handle != nullptr;
    }
    const void *symbol(const char *name) const { return dlsym(handle, name); }
    void close()
    {
        if (handle)
            dlclose(handle);
        handle = nullptr;
    }
#endif
};

void pumpMainThreadMessages()
{
#ifdef _WIN32
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
#endif
}

// --- the host: offers no extensions, which every plugin must tolerate -------

std::atomic<bool> g_callbackRequested{false};

const void *CLAP_ABI hostGetExtension(const clap_host_t *, const char *) { return nullptr; }
void CLAP_ABI hostRequestRestart(const clap_host_t *) {}
void CLAP_ABI hostRequestProcess(const clap_host_t *) {}
void CLAP_ABI hostRequestCallback(const clap_host_t *) { g_callbackRequested = true; }

const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "adi-surge clap_smoke", "adi-surge",
                           "",                "0.1",   hostGetExtension,       hostRequestRestart,
                           hostRequestProcess, hostRequestCallback};

// --- events ----------------------------------------------------------------

struct EventList
{
    std::vector<clap_event_note_t> notes;
};

uint32_t CLAP_ABI eventsSize(const clap_input_events_t *list)
{
    return static_cast<uint32_t>(static_cast<const EventList *>(list->ctx)->notes.size());
}

const clap_event_header_t *CLAP_ABI eventsGet(const clap_input_events_t *list, uint32_t index)
{
    return &static_cast<const EventList *>(list->ctx)->notes[index].header;
}

bool CLAP_ABI eventsPush(const clap_output_events_t *, const clap_event_header_t *) { return true; }

clap_event_note_t makeNote(uint16_t type)
{
    clap_event_note_t n{};
    n.header.size = sizeof(n);
    n.header.time = 0;
    n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    n.header.type = type;
    n.header.flags = 0;
    n.note_id = -1;
    n.port_index = 0;
    n.channel = 0;
    n.key = kKey;
    n.velocity = 0.8;
    return n;
}

// --- audio ports -------------------------------------------------------------

struct Port
{
    std::vector<std::vector<float>> channels;
    std::vector<float *> pointers;
    clap_audio_buffer_t buffer{};

    explicit Port(uint32_t channelCount)
        : channels(channelCount, std::vector<float>(kBlock, 0.0f)), pointers(channelCount)
    {
        for (uint32_t c = 0; c < channelCount; ++c)
            pointers[c] = channels[c].data();
        buffer.data32 = pointers.data();
        buffer.channel_count = channelCount;
    }
};

std::vector<Port> makePorts(const clap_plugin_t *plugin, bool isInput)
{
    std::vector<Port> ports;
    auto *ext = static_cast<const clap_plugin_audio_ports_t *>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    if (!ext)
    {
        if (!isInput)
            ports.emplace_back(2);
        return ports;
    }
    const uint32_t count = ext->count(plugin, isInput);
    ports.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        clap_audio_port_info_t info{};
        ext->get(plugin, i, isInput, &info);
        std::printf("  %s port %u: \"%s\", %u ch\n", isInput ? "in " : "out", i, info.name,
                    info.channel_count);
        ports.emplace_back(info.channel_count);
    }
    return ports;
}

// --- analysis ----------------------------------------------------------------

double rms(const std::vector<float> &x, size_t begin, size_t end)
{
    double sum = 0.0;
    for (size_t i = begin; i < end; ++i)
        sum += double(x[i]) * double(x[i]);
    return end > begin ? std::sqrt(sum / double(end - begin)) : 0.0;
}

double peak(const std::vector<float> &x, size_t begin, size_t end)
{
    double p = 0.0;
    for (size_t i = begin; i < end; ++i)
        p = std::max(p, double(std::fabs(x[i])));
    return p;
}

// YIN (de Cheveigne & Kawahara 2002): cumulative-mean-normalised difference,
// first dip below the threshold, refined by parabolic interpolation.
double estimatePitch(const std::vector<float> &x, size_t begin, size_t length)
{
    const size_t minLag = kSampleRate / 2000, maxLag = kSampleRate / 50;
    if (begin + length > x.size() || length <= maxLag)
        return 0.0;
    const size_t window = length - maxLag;

    std::vector<double> d(maxLag + 1, 0.0);
    for (size_t tau = 1; tau <= maxLag; ++tau)
        for (size_t j = 0; j < window; ++j)
        {
            const double diff = double(x[begin + j]) - double(x[begin + j + tau]);
            d[tau] += diff * diff;
        }

    std::vector<double> cmnd(maxLag + 1, 1.0);
    double running = 0.0;
    for (size_t tau = 1; tau <= maxLag; ++tau)
    {
        running += d[tau];
        cmnd[tau] = running > 0.0 ? d[tau] * double(tau) / running : 1.0;
    }

    size_t best = 0;
    for (size_t tau = minLag; tau < maxLag; ++tau)
        if (cmnd[tau] < 0.1 && cmnd[tau] <= cmnd[tau + 1])
        {
            best = tau;
            break;
        }
    if (best == 0)
        best = size_t(std::min_element(cmnd.begin() + std::ptrdiff_t(minLag), cmnd.end()) -
                      cmnd.begin());

    double refined = double(best);
    if (best > 1 && best < maxLag)
    {
        const double a = cmnd[best - 1], b = cmnd[best], c = cmnd[best + 1];
        const double denom = a - 2.0 * b + c;
        if (denom != 0.0)
            refined += 0.5 * (a - c) / denom;
    }
    return double(kSampleRate) / refined;
}

bool writeWav(const std::string &path, const std::vector<float> &l, const std::vector<float> &r)
{
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f)
        return false;
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
    const uint32_t frames = uint32_t(l.size()), dataBytes = frames * 4;
    f.write("RIFF", 4);
    u32(36 + dataBytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1); // PCM
    u16(2);
    u32(kSampleRate);
    u32(kSampleRate * 4);
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
        for (float s : {l[i], r[i]})
        {
            const float clamped = std::clamp(s, -1.0f, 1.0f);
            u16(uint16_t(int16_t(std::lround(clamped * 32767.0f))));
        }
    return bool(f);
}

// --- the render, on its own thread -------------------------------------------

struct Render
{
    std::vector<float> left, right;
    uint32_t processErrors = 0;
    bool started = false;
    std::atomic<bool> done{false};
};

void renderThread(const clap_plugin_t *plugin, std::vector<Port> *inputs,
                  std::vector<Port> *outputs, Render *out)
{
    out->started = plugin->start_processing(plugin);
    if (out->started)
    {
        const uint32_t preBlocks = uint32_t(kPreSeconds * kSampleRate / kBlock);
        const uint32_t holdBlocks = uint32_t(kHoldSeconds * kSampleRate / kBlock);
        const uint32_t tailBlocks = uint32_t(kTailSeconds * kSampleRate / kBlock);
        const uint32_t total = preBlocks + holdBlocks + tailBlocks;

        std::vector<clap_audio_buffer_t> inBufs, outBufs;
        for (auto &p : *inputs)
            inBufs.push_back(p.buffer);
        for (auto &p : *outputs)
            outBufs.push_back(p.buffer);

        EventList events;
        clap_input_events_t in{&events, eventsSize, eventsGet};
        clap_output_events_t sink{nullptr, eventsPush};

        clap_process_t proc{};
        proc.frames_count = kBlock;
        proc.transport = nullptr;
        proc.audio_inputs = inBufs.empty() ? nullptr : inBufs.data();
        proc.audio_inputs_count = uint32_t(inBufs.size());
        proc.audio_outputs = outBufs.empty() ? nullptr : outBufs.data();
        proc.audio_outputs_count = uint32_t(outBufs.size());
        proc.in_events = &in;
        proc.out_events = &sink;

        for (uint32_t block = 0; block < total; ++block)
        {
            events.notes.clear();
            if (block == preBlocks)
                events.notes.push_back(makeNote(CLAP_EVENT_NOTE_ON));
            if (block == preBlocks + holdBlocks)
                events.notes.push_back(makeNote(CLAP_EVENT_NOTE_OFF));

            for (auto &p : *outputs)
                for (auto &ch : p.channels)
                    std::fill(ch.begin(), ch.end(), 0.0f);

            proc.steady_time = int64_t(block) * kBlock;
            if (plugin->process(plugin, &proc) == CLAP_PROCESS_ERROR)
                ++out->processErrors;

            if (!outputs->empty())
            {
                const auto &main = (*outputs)[0].channels;
                out->left.insert(out->left.end(), main[0].begin(), main[0].end());
                const auto &r = main.size() > 1 ? main[1] : main[0];
                out->right.insert(out->right.end(), r.begin(), r.end());
            }
        }
        plugin->stop_processing(plugin);
    }
    out->done = true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: clap_smoke <plugin.clap> [--id <plugin-id>] [--wav <out.wav>]\n");
        return 2;
    }
    const char *path = argv[1];
    std::string wantId = "org.surge-synth-team.surge-xt";
    std::string wavPath;
    for (int i = 2; i + 1 < argc; i += 2)
    {
        const std::string flag = argv[i];
        if (flag == "--id")
            wantId = argv[i + 1];
        else if (flag == "--wav")
            wavPath = argv[i + 1];
    }

    std::printf("clap_smoke: %s\n", path);
    Library lib;
    if (!lib.open(path))
    {
        std::printf("  FAIL could not load the library\n");
        return 2;
    }
    auto *entry = static_cast<const clap_plugin_entry_t *>(lib.symbol("clap_entry"));
    if (!entry || !clap_version_is_compatible(entry->clap_version) || !entry->init(path))
    {
        std::printf("  FAIL no compatible clap_entry, or init() refused\n");
        return 2;
    }
    auto *factory =
        static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory)
    {
        std::printf("  FAIL no plugin factory\n");
        return 2;
    }

    const clap_plugin_descriptor_t *desc = nullptr;
    const uint32_t count = factory->get_plugin_count(factory);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto *d = factory->get_plugin_descriptor(factory, i);
        std::printf("  factory[%u]: %s  \"%s\" %s\n", i, d->id, d->name,
                    d->version ? d->version : "");
        if (wantId == d->id)
            desc = d;
    }
    const clap_plugin_t *plugin =
        desc ? factory->create_plugin(factory, &kHost, desc->id) : nullptr;
    if (!plugin || !plugin->init(plugin))
    {
        std::printf("  FAIL could not create and init %s\n", wantId.c_str());
        return 2;
    }

    auto inputs = makePorts(plugin, true);
    auto outputs = makePorts(plugin, false);
    check(!outputs.empty() && outputs[0].channels.size() == 2, "main output is stereo",
          std::to_string(outputs.size()) + " output port(s)");

    if (!plugin->activate(plugin, double(kSampleRate), 1, kBlock))
    {
        std::printf("  FAIL activate() refused\n");
        plugin->destroy(plugin);
        return 2;
    }

    Render render;
    std::thread audio(renderThread, plugin, &inputs, &outputs, &render);
    while (!render.done)
    {
        pumpMainThreadMessages();
        if (g_callbackRequested.exchange(false))
            plugin->on_main_thread(plugin);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    audio.join();

    check(render.started, "start_processing() accepted", "");
    check(render.processErrors == 0, "process() never returned ERROR",
          std::to_string(render.processErrors) + " error block(s)");

    const auto &L = render.left, &R = render.right;
    const size_t noteOn = size_t(kPreSeconds * kSampleRate / kBlock) * kBlock;
    const size_t noteOff = noteOn + size_t(kHoldSeconds * kSampleRate / kBlock) * kBlock;
    const size_t settled = noteOn + kSampleRate / 20; // skip the attack: 50 ms
    const size_t tailFrom = L.size() - kSampleRate / 4;

    if (!L.empty() && noteOff < tailFrom)
    {
        bool finite = true;
        for (size_t i = 0; i < L.size(); ++i)
            finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]);
        check(finite, "no NaN or Inf in the output", std::to_string(L.size()) + " frames");

        const double before = std::max(peak(L, 0, noteOn), peak(R, 0, noteOn));
        check(dbfs(before) < -80.0, "silent before the note", fmt("peak %.1f dBFS", dbfs(before)));

        const double heldL = rms(L, settled, noteOff), heldR = rms(R, settled, noteOff);
        check(dbfs(heldL) > -40.0 && dbfs(heldR) > -40.0, "sound while the note is held",
              fmt("RMS L %.1f dBFS", dbfs(heldL)) + fmt(", R %.1f dBFS", dbfs(heldR)));

        const double hz = estimatePitch(L, noteOn + kSampleRate / 5, 4096);
        check(std::fabs(hz / kExpectedHz - 1.0) < 0.01, "pitch of key 69 is 440 Hz",
              fmt("measured %.2f Hz", hz));

        const double tail = std::max(rms(L, tailFrom, L.size()), rms(R, tailFrom, R.size()));
        const double held = std::max(heldL, heldR);
        check(dbfs(tail) < dbfs(held) - 40.0, "silent again after the release",
              fmt("tail %.1f dBFS", dbfs(tail)) + fmt(" vs held %.1f dBFS", dbfs(held)));
    }
    else
    {
        check(false, "rendered audio", "no output frames captured");
    }

    if (!wavPath.empty())
        check(writeWav(wavPath, L, R), "wrote the WAV", wavPath);

    plugin->deactivate(plugin);
    pumpMainThreadMessages();
    plugin->destroy(plugin);
    entry->deinit();
    lib.close();

    std::printf("clap_smoke: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
