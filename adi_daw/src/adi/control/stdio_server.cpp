// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/stdio_server.hpp"

#include <istream>
#include <ostream>
#include <streambuf>

namespace adi::control {

bool readBoundedLine(std::istream& in, std::string& line, std::size_t maxBytes, bool& tooLong) {
    line.clear();
    tooLong = false;
    std::streambuf* buf = in.rdbuf();
    if (!buf) return false;
    bool any = false;
    for (;;) {
        const auto c = buf->sbumpc();
        if (c == std::char_traits<char>::eof()) {
            in.setstate(std::ios::eofbit);
            break;
        }
        any = true;
        const char ch = std::char_traits<char>::to_char_type(c);
        if (ch == '\n') break;
        if (tooLong) continue;   // consume the rest of an over-long line
        if (line.size() >= maxBytes) {
            // One CR at the bound is the CRLF of a line of exactly maxBytes:
            // keep it for the strip below, so a "\r\n" line frames as a "\n" one does.
            if (ch == '\r' && line.size() == maxBytes) {
                line.push_back(ch);
                continue;
            }
            tooLong = true;
            line.clear();
            continue;
        }
        line.push_back(ch);
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return any;
}

std::size_t runStdio(std::istream& in, std::ostream& out, McpEndpoint& endpoint,
                     McpEndpoint::Session& session, const WireLimits& limits, std::ostream* log) {
    std::size_t handled = 0;
    std::string line;
    bool tooLong = false;
    endpoint.noteConnection(session, "connect");
    while (readBoundedLine(in, line, limits.maxMessageBytes, tooLong)) {
        if (tooLong) {
            // The id is in the part we did not keep, so the refusal has none.
            out << encodeError(nullptr, {errc::InvalidRequest,
                                         "message larger than " + std::to_string(limits.maxMessageBytes) +
                                             " bytes",
                                         nullptr})
                << '\n'
                << std::flush;
            if (log) *log << "adi-mcp: refused an over-long line\n";
            continue;
        }
        if (line.find_first_not_of(" \t") == std::string::npos) continue;   // blank lines are not messages
        const Incoming message = parseMessage(line, limits);
        ++handled;
        if (auto reply = endpoint.handle(message, session)) out << *reply << '\n' << std::flush;
        if (message.kind == Incoming::Kind::Invalid && log)
            *log << "adi-mcp: refused a message: " << message.error.message << '\n';
    }
    endpoint.noteConnection(session, "disconnect");
    return handled;
}

}  // namespace adi::control
