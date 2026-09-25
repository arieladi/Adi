// SPDX-License-Identifier: GPL-3.0-or-later

#include "airwindows_clap.hpp"

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    adi::airwindows::entryInit,
    adi::airwindows::entryDeinit,
    adi::airwindows::entryGetFactory,
};
