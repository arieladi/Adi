// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "EngineBank.h"
#include "BlockEngine.h"
#include "Latency.h"
#include "StylePrograms.h"

namespace adi::mix::limiter
{
inline Frame processStyle(Style style, EngineBank& engine, const adi::mix::limiter::programs::Controls& controls,
                          const Frame& audio, const Frame& key)
{
    switch (style)
    {
        case Style::transparent: return adi::mix::limiter::programs::processTransparent(engine, controls, audio, key);
        case Style::punchy: return adi::mix::limiter::programs::processPunchy(engine, controls, audio, key);
        case Style::dynamic: return adi::mix::limiter::programs::processDynamic(engine, controls, audio, key);
        case Style::allround: return adi::mix::limiter::programs::processAllround(engine, controls, audio, key);
        case Style::aggressive: return adi::mix::limiter::programs::processAggressive(engine, controls, audio, key);
        case Style::modern: return adi::mix::limiter::programs::processModern(engine, controls, audio, key);
        case Style::bus: return adi::mix::limiter::programs::processBus(engine, controls, audio, key);
        case Style::safe: return adi::mix::limiter::programs::processSafe(engine, controls, audio, key);
        default: throw std::invalid_argument("Unknown engine");
    }
}
template<typename Blocks>
inline blocked::Frame processStyleBlock(Style style, Blocks& engine, const blocked::Controls& controls,
                          const blocked::Frame& audio, const blocked::Frame& key)
{
    switch (style)
    {
        case Style::transparent: return engine.capture(adi::mix::limiter::programs::processTransparent(engine, controls, audio, key));
        case Style::punchy: return engine.capture(adi::mix::limiter::programs::processPunchy(engine, controls, audio, key));
        case Style::dynamic: return engine.capture(adi::mix::limiter::programs::processDynamic(engine, controls, audio, key));
        case Style::allround: return engine.capture(adi::mix::limiter::programs::processAllround(engine, controls, audio, key));
        case Style::aggressive: return engine.capture(adi::mix::limiter::programs::processAggressive(engine, controls, audio, key));
        case Style::modern: return engine.capture(adi::mix::limiter::programs::processModern(engine, controls, audio, key));
        case Style::bus: return engine.capture(adi::mix::limiter::programs::processBus(engine, controls, audio, key));
        case Style::safe: return engine.capture(adi::mix::limiter::programs::processSafe(engine, controls, audio, key));
        default: throw std::invalid_argument("Unknown engine");
    }
}
}
