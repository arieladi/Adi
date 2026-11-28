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
        "('transport.metronome','transport.metronomeOptions','transport.loop',"
        "'transport.countIn','transport.songStart','transport.punch',"
        "'transport.launchQuantization','session.followActionsEnabled')");
    while(rows.executeStep()) {
        try {
            const auto value=Payload::parse(rows.getColumn(1).getString());
            if(rows.getColumn(0).getString()=="transport.metronome")result.metronome=value.get<bool>();
            else if (rows.getColumn(0).getString() == "transport.metronomeOptions") {
                if (const auto item = value.find("recordingOnly"); item != value.end() && item->is_boolean())
                    result.metronomeOptions.recordingOnly = item->get<bool>();
                if (const auto item = value.find("eighthNotes"); item != value.end() && item->is_boolean())
                    result.metronomeOptions.eighthNotes = item->get<bool>();
                if (const auto item = value.find("sound"); item != value.end() && item->is_number_integer() &&
                    *item >= 0 && *item <= static_cast<int>(MetronomeSound::Bell))
                    result.metronomeOptions.sound = static_cast<MetronomeSound>(item->get<int>());
            } else if (rows.getColumn(0).getString() == "transport.countIn") {
                if (!value.is_number_integer() || value < 0 || value > 4)
                    continue;
                const auto bars = value.get<std::int32_t>();
                if (bars == 0 || bars == 1 || bars == 2 || bars == 4)
                    result.countInBars = bars;
            } else if (rows.getColumn(0).getString() == "transport.songStart") {
                if (!value.is_number_integer() || value < 0 || value > INT64_MAX)
                    continue;
                result.songStartTicks = value.get<std::int64_t>();
            } else if (rows.getColumn(0).getString() == "transport.punch") {
                const auto in = value.at("in").get<bool>(), out = value.at("out").get<bool>();
                result.punchIn = in;
                result.punchOut = out;
            } else if (rows.getColumn(0).getString() == "transport.launchQuantization") {
                if (value.is_number_integer() && value >= 0 &&
                    value <= static_cast<std::int32_t>(LaunchQuantization::ThirtySecond))
                    result.launchQuantization = static_cast<LaunchQuantization>(value.get<std::int32_t>());
            } else if (rows.getColumn(0).getString() == "session.followActionsEnabled") {
                if (value.is_boolean())
                    result.followActionsEnabled = value.get<bool>();
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
