// Adi Ariel MIDI Control - native helper (cross-platform: Windows + macOS)
//
// Receives JSON commands from the Stream Deck plugin front-end over a localhost
// websocket and:
//   * emits MIDI to a virtual port named "Stream Deck MIDI Control"
//       - Windows: teVirtualMIDI (driver ships with loopMIDI)
//       - macOS:   CoreMIDI virtual source (built in - no driver to install)
//   * injects OS keystrokes for the num-pad
//       - Windows: SendInput (numpad virtual-keys)
//       - macOS:   CGEvent (requires Accessibility permission for this binary)
//
// JSON protocol (one object per message):
//   { "op":"noteOn",  "note":36, "vel":110, "ch":1 }
//   { "op":"noteOff", "note":36, "ch":1 }
//   { "op":"cc",      "cc":20,   "val":64, "ch":1 }
//   { "op":"key",     "key":"7" }        // "0".."9" . + - x / Enter Clear
//
// Dependencies: IXWebSocket, nlohmann/json (both via CMake FetchContent).
//   Windows also needs the teVirtualMIDI SDK in third_party/teVirtualMIDI.
//   macOS uses the system CoreMIDI / ApplicationServices frameworks (no extras).

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>

#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
  #define NOMINMAX
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include "teVirtualMIDI.h"
#elif defined(__APPLE__)
  #include <CoreMIDI/CoreMIDI.h>
  #include <CoreFoundation/CoreFoundation.h>
  #include <ApplicationServices/ApplicationServices.h>
#else
  #error "Unsupported platform: the Stream Deck app runs only on Windows and macOS."
#endif

using json = nlohmann::json;

static const int   LISTEN_PORT = 9234;
static const char* LISTEN_HOST = "127.0.0.1";   // localhost only
static const char* PORT_NAME   = "Stream Deck MIDI Control";

/* ------------------------------ MIDI backend ------------------------------ */
// One small class with a per-platform implementation. Other apps (Ableton) see
// the data we send as a MIDI *input* (a virtual source they can subscribe to).

class MidiPort {
public:
    bool open();
    void close();

    void noteOn(int note, int vel, int ch) { send3(0x90 | chan(ch), clamp7(note), clamp7(vel)); }
    void noteOff(int note, int ch)          { send3(0x80 | chan(ch), clamp7(note), 0); }
    void cc(int controller, int value, int ch) { send3(0xB0 | chan(ch), clamp7(controller), clamp7(value)); }

private:
    void send3(uint8_t status, uint8_t d1, uint8_t d2);

    static uint8_t chan(int ch)  { return (uint8_t)((ch < 1 ? 1 : (ch > 16 ? 16 : ch)) - 1); }
    static uint8_t clamp7(int v) { return (uint8_t)(v < 0 ? 0 : (v > 127 ? 127 : v)); }

    std::mutex mtx_;
#if defined(_WIN32)
    LPVM_MIDI_PORT  port_ = nullptr;
#elif defined(__APPLE__)
    MIDIClientRef   client_ = 0;
    MIDIEndpointRef source_ = 0;
#endif
};

#if defined(_WIN32)

bool MidiPort::open() {
    // TX-only: data we send appears as a MIDI input to other apps (Ableton).
    wchar_t name[] = L"Stream Deck MIDI Control";
    port_ = virtualMIDICreatePortEx2(name, nullptr, 0, 0, TE_VM_FLAGS_INSTANTIATE_TX_ONLY);
    return port_ != nullptr;
}
void MidiPort::close() {
    if (port_) { virtualMIDIClosePort(port_); port_ = nullptr; }
}
void MidiPort::send3(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!port_) return;
    BYTE buf[3] = { status, d1, d2 };
    std::lock_guard<std::mutex> lk(mtx_);
    virtualMIDISendData(port_, buf, 3);
}

#elif defined(__APPLE__)

bool MidiPort::open() {
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, PORT_NAME, kCFStringEncodingUTF8);
    OSStatus s = MIDIClientCreate(name, nullptr, nullptr, &client_);
    if (s == noErr) s = MIDISourceCreate(client_, name, &source_);
    CFRelease(name);
    return s == noErr;
}
void MidiPort::close() {
    if (source_) { MIDIEndpointDispose(source_); source_ = 0; }
    if (client_) { MIDIClientDispose(client_);   client_ = 0; }
}
void MidiPort::send3(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!source_) return;
    Byte storage[64];
    MIDIPacketList* pktList = reinterpret_cast<MIDIPacketList*>(storage);
    MIDIPacket* pkt = MIDIPacketListInit(pktList);
    Byte data[3] = { status, d1, d2 };
    std::lock_guard<std::mutex> lk(mtx_);
    pkt = MIDIPacketListAdd(pktList, sizeof(storage), pkt, 0, sizeof(data), data);
    if (pkt) MIDIReceived(source_, pktList);
}

#endif

static MidiPort g_midi;

/* --------------------------- Keystroke injection -------------------------- */

#if defined(_WIN32)

static WORD vkForKey(const std::string& k) {
    if (k.size() == 1 && k[0] >= '0' && k[0] <= '9')
        return (WORD)(VK_NUMPAD0 + (k[0] - '0'));
    if (k == ".")     return VK_DECIMAL;
    if (k == "+")     return VK_ADD;
    if (k == "-")     return VK_SUBTRACT;
    if (k == "x")     return VK_MULTIPLY;
    if (k == "/")     return VK_DIVIDE;
    if (k == "Enter") return VK_RETURN;
    if (k == "Clear") return VK_ESCAPE;   // calculator "C" / cancel
    return 0;
}

static void pressKey(const std::string& k) {
    WORD vk = vkForKey(k);
    if (vk == 0) return;
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = vk; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

#elif defined(__APPLE__)

// Carbon/HIToolbox virtual key codes. Digits + operators use the numeric keypad
// to mirror the Windows VK_NUMPAD* behavior; "Clear" maps to Escape like Windows.
static bool keyCodeFor(const std::string& k, CGKeyCode& out) {
    static const struct { const char* s; CGKeyCode c; } map[] = {
        {"0",82},{"1",83},{"2",84},{"3",85},{"4",86},
        {"5",87},{"6",88},{"7",89},{"8",91},{"9",92},
        {".",65},                 // kVK_ANSI_KeypadDecimal
        {"+",69},{"-",78},{"x",67},{"/",75},  // Keypad Plus / Minus / Multiply / Divide
        {"Enter",76},             // kVK_ANSI_KeypadEnter
        {"Clear",53}              // kVK_Escape
    };
    for (const auto& e : map) if (k == e.s) { out = e.c; return true; }
    return false;
}

static void pressKey(const std::string& k) {
    CGKeyCode code;
    if (!keyCodeFor(k, code)) return;
    CGEventRef down = CGEventCreateKeyboardEvent(nullptr, code, true);
    CGEventRef up   = CGEventCreateKeyboardEvent(nullptr, code, false);
    if (down) { CGEventPost(kCGHIDEventTap, down); CFRelease(down); }
    if (up)   { CGEventPost(kCGHIDEventTap, up);   CFRelease(up);   }
}

#endif

/* ----------------------------- Message handler ---------------------------- */

static void handleMessage(const std::string& text) {
    json j;
    try { j = json::parse(text); }
    catch (...) { return; }

    const std::string op = j.value("op", "");
    const int ch = j.value("ch", 1);

    if (op == "noteOn") {
        g_midi.noteOn(j.value("note", 60), j.value("vel", 110), ch);
    } else if (op == "noteOff") {
        g_midi.noteOff(j.value("note", 60), ch);
    } else if (op == "cc") {
        g_midi.cc(j.value("cc", 20), j.value("val", 0), ch);
    } else if (op == "key") {
        pressKey(j.value("key", std::string()));
    }
}

/* ---------------------------------- main ---------------------------------- */

int main() {
    if (!g_midi.open()) {
#if defined(_WIN32)
        std::cerr << "[AdiArielMIDI] Failed to create virtual port '" << PORT_NAME
                  << "'. Is the teVirtualMIDI driver installed (loopMIDI)?" << std::endl;
#else
        std::cerr << "[AdiArielMIDI] Failed to create CoreMIDI virtual source '" << PORT_NAME
                  << "'." << std::endl;
#endif
        return 1;
    }
    std::cout << "[AdiArielMIDI] Virtual MIDI port '" << PORT_NAME << "' created." << std::endl;

    ix::WebSocketServer server(LISTEN_PORT, LISTEN_HOST);

    server.setOnClientMessageCallback(
        [](std::shared_ptr<ix::ConnectionState> /*state*/,
           ix::WebSocket& webSocket,
           const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Message) {
                handleMessage(msg->str);
            } else if (msg->type == ix::WebSocketMessageType::Open) {
                std::cout << "[AdiArielMIDI] Plugin front-end connected." << std::endl;
            } else if (msg->type == ix::WebSocketMessageType::Close) {
                std::cout << "[AdiArielMIDI] Plugin front-end disconnected." << std::endl;
            }
            (void)webSocket;
        });

    auto res = server.listen();
    if (!res.first) {
        std::cerr << "[AdiArielMIDI] Listen failed: " << res.second << std::endl;
        g_midi.close();
        return 1;
    }

    server.start();
    std::cout << "[AdiArielMIDI] Listening on ws://" << LISTEN_HOST << ":" << LISTEN_PORT << std::endl;
    server.wait();   // block forever

    g_midi.close();
    return 0;
}
