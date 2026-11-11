// SPDX-License-Identifier: AGPL-3.0-or-later
//
// adi-mcp's loop: newline-delimited JSON-RPC on stdin, one reply line per
// request on stdout, nothing else on stdout ever (MCP stdio transport).
// Diagnostics go to the log stream (stderr in adi-mcp).
//
// A line is read with a byte bound: an over-long line is consumed to its end,
// refused, and the loop carries on. Messages are handled one at a time, in
// order, which is ADR-0168's one lane per project.

#pragma once

#include "adi/control/jsonrpc.hpp"
#include "adi/control/mcp.hpp"

#include <cstddef>
#include <iosfwd>
#include <string>

namespace adi::control {

/// Reads one line (without its '\n', and without a trailing '\r'). False at
/// end of input with nothing read. A line longer than `maxBytes` (counted
/// without its line ending, so a CRLF line of exactly `maxBytes` is whole) is
/// read to its end and discarded, with `tooLong` set.
bool readBoundedLine(std::istream& in, std::string& line, std::size_t maxBytes, bool& tooLong);

/// Runs until end of input. Returns the number of messages handled.
std::size_t runStdio(std::istream& in, std::ostream& out, McpEndpoint& endpoint,
                     McpEndpoint::Session& session, const WireLimits& limits, std::ostream* log);

}  // namespace adi::control
