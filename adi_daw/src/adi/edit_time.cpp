// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/edit_time.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace adi::edit {
std::int64_t addTime(std::int64_t a,std::int64_t b) {
    if((b>0 && a>std::numeric_limits<std::int64_t>::max()-b) ||
       (b<0 && a<std::numeric_limits<std::int64_t>::min()-b))throw std::runtime_error("edit time overflows signed 64-bit");
    return a+b;
}
std::int64_t nanosFromSeconds(double seconds) {
    const long double value=static_cast<long double>(seconds)*1e9L;
    if(!std::isfinite(value) || value<0 || value>static_cast<long double>(std::numeric_limits<std::int64_t>::max())-4096)
        throw std::runtime_error("edit time is outside nanosecond range");
    return static_cast<std::int64_t>(std::llround(value));
}
TimeMap::TimeMap(SQLite::Database& db) {
    SQLite::Statement st(db,"SELECT pos_ticks,bpm,curve FROM tempo_map ORDER BY pos_ticks,id");
    while(st.executeStep()) {
        const auto position=st.getColumn(0).getInt64();const auto bpm=st.getColumn(1).getDouble();
        if(position<0 || !std::isfinite(bpm) || bpm<=0)throw std::runtime_error("edit needs a valid tempo map");
        fastest_=std::max(fastest_,bpm);tempo_.events.push_back({position,bpm,st.getColumn(2).getInt()});
    }
    if(tempo_.events.empty() || tempo_.events.front().posTicks!=0)tempo_.events.insert(tempo_.events.begin(),{0,120,0});
}
double TimeMap::seconds(std::int64_t ticks)const {
    if(ticks<0)throw std::runtime_error("edit ticks must be nonnegative");
    const auto result=tempo_.ticksToSeconds(ticks);
    if(!std::isfinite(result) || result<0)throw std::runtime_error("edit tempo conversion is outside range");
    return result;
}
double TimeMap::durationSeconds(std::int64_t origin,std::int64_t ticks)const {
    if(origin<0 || ticks<0)throw std::runtime_error("edit duration must be nonnegative");
    return seconds(addTime(origin,ticks))-seconds(origin);
}
std::int64_t TimeMap::nanos(std::int64_t ticks)const {return nanosFromSeconds(seconds(ticks));}
std::int64_t TimeMap::ticks(std::int64_t nanos)const {
    if(nanos<0)throw std::runtime_error("edit nanoseconds must be nonnegative");
    const long double bound=static_cast<long double>(nanos)/1e9L*fastest_*textproj::kPPQ/60;
    if(!std::isfinite(bound) || bound>static_cast<long double>(std::numeric_limits<std::int64_t>::max())-4096)
        throw std::runtime_error("edit time is outside tick range");
    return tempo_.secondsToTicks(static_cast<double>(nanos)*1e-9);
}
std::int64_t TimeMap::duration(std::int64_t origin,std::int64_t ticks)const {
    if(origin<0 || ticks<0)throw std::runtime_error("edit duration must be nonnegative");
    return nanosFromSeconds(durationSeconds(origin,ticks));
}
double TimeMap::bpmBefore(std::int64_t ticks)const {
    if(ticks<0)throw std::runtime_error("edit ticks must be nonnegative");
    double result=tempo_.events.front().bpm;
    for(const auto& event:tempo_.events) {
        if(event.posTicks>=ticks)break;
        result=event.bpm;
    }
    return result;
}
} // namespace adi::edit
