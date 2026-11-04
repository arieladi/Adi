// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/transport_settings.hpp"
#include "adi/store.hpp"
#include "adi/ops.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
namespace adi {
TransportSettings readTransportSettings(const Store& store) {
    TransportSettings result;
    SQLite::Statement rows(const_cast<SQLite::Database&>(store.db()),"SELECT key,value FROM session_state WHERE key IN ('transport.metronome','transport.loop')");
    while(rows.executeStep()) {
        try {
            const auto value=Payload::parse(rows.getColumn(1).getString());
            if(rows.getColumn(0).getString()=="transport.metronome")result.metronome=value.get<bool>();
            else {
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
