// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/transport_settings.hpp"
#include "adi/store.hpp"
#include "adi/ops.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
namespace adi {
TransportSettings readTransportSettings(const Store& store) {
    TransportSettings result;
    SQLite::Statement rows(
        const_cast<SQLite::Database &>(store.db()),
        "SELECT key,value FROM session_state WHERE key IN "
        "('transport.metronome','transport.loop','transport.countIn','transport.songStart')");
    while(rows.executeStep()) {
        try {
            const auto value=Payload::parse(rows.getColumn(1).getString());
            if(rows.getColumn(0).getString()=="transport.metronome")result.metronome=value.get<bool>();
            else if (rows.getColumn(0).getString() == "transport.countIn") {
                if (!value.is_number_integer() || value < 0 || value > 4)
                    continue;
                const auto bars = value.get<std::int32_t>();
                if (bars == 0 || bars == 1 || bars == 2 || bars == 4)
                    result.countInBars = bars;
            } else if (rows.getColumn(0).getString() == "transport.songStart") {
                if (!value.is_number_integer() || value < 0 || value > INT64_MAX)
                    continue;
                result.songStartTicks = value.get<std::int64_t>();
            } else {
                const auto on=value.at("on").get<bool>();
                const auto start=value.at("start").get<std::int64_t>(),end=value.at("end").get<std::int64_t>();
                if(start<0 || end<=start)continue;
                result.loopEnabled=on;result.loopStartTicks=start;result.loopEndTicks=end;result.hasLoop=true;
            }
        }catch(const std::exception&){/* corrupt/unknown session metadata uses defaults */}
    }
    return result;
}
}
