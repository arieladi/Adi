// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/scale_context.hpp"
namespace adi::engine {
struct ScalaImport {
    std::optional<TuningDefinition> definition;
    std::string error;
    std::size_t line=0; // physical error/reference line; zero for pre-source identity/input budget
};
// Owner-only parsing of already-read .scl bytes. Explicit project identity and
// display name; no path, Store mutation, keyboard map or absolute tonic guess.
// The last interval is the period; degree zero is inserted at unison.
ScalaImport parseScala(std::string_view bytes,std::int64_t id,std::string_view name);
} // namespace adi::engine
