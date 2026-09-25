// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/summing_flavors.hpp"

#include <array>

namespace adi {

namespace {

constexpr std::array<SummingFlavor, 8> kFlavors{{
    {"console9", "Console9", "Console9Channel", "Console9Buss"},
    {"console.la", "Console LA (Quad Eight)", "ConsoleLAChannel", "ConsoleLABuss"},
    {"console.mc", "Console MC (MCI, bright)", "ConsoleMCChannel", "ConsoleMCBuss"},
    {"console.md", "Console MD (MCI)", "ConsoleMDChannel", "ConsoleMDBuss"},
    {"purest3", "PurestConsole3", "PurestConsole3Channel", "PurestConsole3Buss"},
    {"pd", "PD (Console5 + PurestDrive)", "PDChannel", "PDBuss"},
    {"c5raw", "C5Raw (the original Console5)", "C5RawChannel", "C5RawBuss"},
    {"atmosphere", "Atmosphere (Console5, distance)", "AtmosphereChannel", "AtmosphereBuss"},
    // NOT EveryConsole (ADR-0174). Its six systems would be flavours, but at
    // the pinned commit its processing reads the type as `(int) A*11.999` --
    // the cast binds first, so every setting below 1.0 plays type 0 -- and no
    // channel/buss pair can be chosen. Its keys are reserved until it is fixed
    // upstream: every.retro, every.sine, every.c6, every.c7, every.bshift,
    // every.czero.
}};

}  // namespace

std::span<const SummingFlavor> summingFlavors() noexcept { return kFlavors; }

const SummingFlavor* findSummingFlavor(std::string_view key) noexcept {
    for (const SummingFlavor& f : kFlavors)
        if (f.key == key) return &f;
    return nullptr;
}

}  // namespace adi
