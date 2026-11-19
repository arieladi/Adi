// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/builtin_catalog.hpp"
#include <array>
#if ADI_WITH_PD
#include "adi_native_pd_patches.hpp"
#endif
namespace adi {
namespace {
#if ADI_WITH_PD
constexpr bool ready=true;
#else
constexpr bool ready=false;
#endif
constexpr std::array<BuiltinInfo,18> entries={{
 {"org.adidaw.builtin.auto-filter","Auto Filter","Audio Effects","EQ & Filters",4,ready,false},
 {"org.adidaw.builtin.chord-comb","Chord Comb","ADI","Audio Effects",2,ready,false},
 {"org.adidaw.builtin.chorus-ensemble","Chorus-Ensemble","Audio Effects","Pitch & Modulation",2,ready,false},
 {"org.adidaw.builtin.color-cab","Color Cab","ADI","Audio Effects",2,ready,false},
 {"org.adidaw.builtin.delay","Delay","Audio Effects","Delay & Loop",2,ready,false},
 {"org.adidaw.builtin.eq-eight","EQ Eight","Audio Effects","EQ & Filters",2,ready,false},
 {"org.adidaw.builtin.echo","Echo","Audio Effects","Delay & Loop",2,ready,false},
 {"org.adidaw.builtin.hybrid-reverb","Hybrid Reverb","Audio Effects","Reverb & Resonance",2,false,false},
 {"org.adidaw.builtin.multiband-dynamics","Multiband Dynamics","Audio Effects","Dynamics",4,ready,false},
 {"org.adidaw.builtin.overdrive","Overdrive","Audio Effects","Drive & Color",2,ready,false},
 {"org.adidaw.builtin.phaser-flanger","Phaser-Flanger","Audio Effects","Pitch & Modulation",2,ready,false},
 {"org.adidaw.builtin.redux","Redux","Audio Effects","Drive & Color",2,ready,false},
 {"org.adidaw.builtin.resonators","Resonators","Audio Effects","Reverb & Resonance",2,ready,false},
 {"org.adidaw.builtin.reverb","Reverb","Audio Effects","Reverb & Resonance",2,ready,false},
 {"org.adidaw.builtin.saturator","Saturator","Audio Effects","Drive & Color",2,ready,false},
 {"org.adidaw.builtin.shifter","Shifter","Audio Effects","Pitch & Modulation",2,ready,true},
 {"org.adidaw.builtin.utility","Utility","Audio Effects","Utilities",2,ready,false},
 {"org.adidaw.builtin.vocoder","Vocoder","Audio Effects","Pitch & Modulation",4,ready,false}
}};
}
std::span<const BuiltinInfo> builtinCatalog() noexcept{return entries;}
const BuiltinInfo* findBuiltin(std::string_view uid) noexcept {
    for(const auto& entry:entries)if(entry.uid==uid)return &entry;
    return nullptr;
}
std::string_view builtinCanvas(std::string_view uid) noexcept {
#if ADI_WITH_PD
    using namespace device::canonical;
    constexpr std::array<std::string_view,18> canvases={autoFilter,chordComb,chorusEnsemble,colorCab,delay,eqEight,echo,
        std::string_view{},multibandDynamics,overdrive,phaserFlanger,redux,resonators,reverb,saturator,shifter,utility,vocoder};
    for(std::size_t i=0;i<entries.size();++i)if(entries[i].uid==uid)return canvases[i];
#else
    (void)uid;
#endif
    return {};
}
}
