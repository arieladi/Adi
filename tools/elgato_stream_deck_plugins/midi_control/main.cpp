// Adi Ariel MIDI Control - native helper
//
// Receives JSON commands from the Stream Deck plugin front-end over a localhost
// websocket and:
//   * emits MIDI to a virtual port named "Stream Deck MIDI Control" (teVirtualMIDI), and
//   * injects OS keystrokes for the num-pad (Windows SendInput).
//
// JSON protocol (one object per message):
//   { "op":"noteOn",  "note":36, "vel":110, "ch":1 }
//   { "op":"noteOff", "note":36, "ch":1 }
//   { "op":"cc",      "cc":20,   "val":64, "ch":1 }
//   { "op":"key",     "key":"7" }        // "0".."9" . + - x / Enter Clear
//
// Dependencies: IXWebSocket, nlohmann/json (both via CMake FetchContent),
//               teVirtualMIDI SDK (third_party/teVirtualMIDI, ships with loopMIDI).

#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>

#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

#include "teVirtualMIDI.h"

using json = nlohmann::json;

static const int    LISTEN_PORT = 9234;
static const char*  LISTEN_HOST = "127.0.0.1";   // localhost only
static const wchar_t* PORT_NAME = L"Stream Deck MIDI Control";

/* ------------------------------ MIDI backend ------------------------------ */

class MidiPort {
public:
    bool open() {
        // TX-only: data we send appears as a MIDI *input* to other apps (Ableton).
        port_ = virtualMIDICreatePortEx2(PORT_NAME, nullptr, 0, 0,
                                         TE_VM_FLAGS_INSTANTIATE_TX_ONLY);
        return port_ != nullptr;
    }

    void close() {
        if (port_) { virtualMIDIClosePort(port_); port_ = nullptr; }
    }

    void send3(uint8_t status, uint8_t d1, uint8_t d2) {
        if (!port_) return;
        BYTE buf[3] = { status, d1, d2 };
        std::lock_guard<std::mutex> lk(mtx_);
        virtualMIDISendData(port_, buf, 3);
    }

    void noteOn(int note, int vel, int ch) {
        send3(0x90 | chan(ch), clamp7(note), clamp7(vel));
    }
    void noteOff(int note, int ch) {
        send3(0x80 | chan(ch), clamp7(note), 0);
    }
    void cc(int controller, int value, int ch) {
        send3(0xB0 | chan(ch), clamp7(controller), clamp7(value));
    }

private:
    static uint8_t chan(int ch) { return (uint8_t)((ch < 1 ? 1 : (ch > 16 ? 16 : ch)) - 1); }
    static uint8_t clamp7(int v) { return (uint8_t)(v < 0 ? 0 : (v > 127 ? 127 : v)); }

    LPVM_MIDI_PORT port_ = nullptr;
    std::mutex     mtx_;
};

static MidiPort g_midi;

/* --------------------------- Keystroke injection -------------------------- */

static WORD vkForKey(const std::string& k) {
    if (k.size() == 1 && k[0] >= '0' && k[0] <= '9')
        return (WORD)(VK_NUMPAD0 + (k[0] - '0'));
    if (k == ".")     return VK_DECIMAL;
    if (k == "+")     return VK_ADD;
    if (k == "-")     return VK_SUBTRACT;
    if (k == "x")     return VK_MULTIPLY;
    if (k == "/")     return VK_DIVIDE;
    if (k == "Enter") return VK_RETURN;
    if (k == "Clear") return VK_ESCAPE;   // calculator "C"/cancel
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
        std::wcerr << L"[AdiArielMIDI] Failed to create virtual port '" << PORT_NAME
                   << L"'. Is the teVirtualMIDI driver installed (loopMIDI)?" << std::endl;
        return 1;
    }
    std::wcout << L"[AdiArielMIDI] Virtual MIDI port '" << PORT_NAME << L"' created." << std::endl;

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
