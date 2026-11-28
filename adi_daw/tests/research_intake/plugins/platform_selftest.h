// Actual platform/trace ownership and bounds checks. Original diagnostic, MIT.
#pragma once
#include "probe_shared.h"
#include "time/probe/time_shared.h"

inline void adi_platform_selftest(void (*check)(bool,const char*)){
    using namespace adi_platform;
    check(posixName("Local\\ADI_Test")=="/adi_a66c23070737961d","POSIX keeper name golden vector");
    const auto name=std::string("Local\\ADI_PLATFORM_\xCE\xA9_")+std::to_string(pid())+"_"+std::to_string(ticks());
    Environment restore("ADI_TIME_MAP");
    check(setEnvironment("ADI_TIME_MAP",name),"platform test environment");
    TimeTrace writer;
    check(!writer.open()&&!writer.enabled(),"missing keeper rejected without creating a map");
    Mapping owner,duplicate,shortOwner;
    check(shortOwner.open(name,256,true),"create deliberately short keeper");
    check(!writer.open(),"undersized keeper rejected");
    shortOwner.close();
    check(owner.open(name,TIME_MAP_BYTES,true),"exclusive keeper create");
    check(!duplicate.open(name,TIME_MAP_BYTES,true),"duplicate keeper rejected");
    check(!writer.open()&&!writer.enabled(),"invalid header rejected and closed");
    auto* header=static_cast<TimeHeader*>(owner.data());
    std::memcpy(header->magic,"ADITIME1",8);header->version=1;
    header->record_size=sizeof(TimeRecord);header->capacity=TIME_CAP+1;
    check(!writer.open(),"incompatible capacity rejected");
    header->capacity=TIME_CAP;header->frequency=frequency();
    check(writer.open(),"valid keeper reopened after failures");
    publish(&header->enabled,1);check(writer.enabled(),"acquire enable state");
    TimeRecord record{};record.qpc=ticks();record.frames=7;record.project_samples=-4800;
    writer.record(record);
    auto* rows=mappedAt<TimeRecord,TIME_HEADER_BYTES>(header);
    check(acquire(&rows[0].committed)==1&&rows[0].frames==7&&rows[0].project_samples==-4800,"release/acquire complete record");
    check(rows[0].pid==pid()&&rows[0].tid==tid()&&rows[0].qpc>=0&&frequency()>0,"honest native clock and identity");
    // No writer is active while this test places the ring at its final slot.
    header->next=static_cast<Atomic64>(TIME_CAP-1);writer.record(record);writer.record(record);
    check(acquire(&header->lost)==1&&acquire(&rows[TIME_CAP-1].committed)==static_cast<Atomic64>(TIME_CAP),"bounded overflow publishes last slot and counts loss");
    check(rows[0].project_samples==-4800,"overflow does not overwrite beginning");
    publish(&header->enabled,0);check(!writer.enabled(),"disable acquire state");
    writer.close();owner.close();
    check(!writer.open(),"keeper cleanup removes named object");
    char16_t string[128]{};text16(string,L"Gain",128);
    check(string[0]==u'G'&&string[3]==u'n'&&string[4]==0,"UTF-16 label units");
    number16(string,.25);double value=0;
    check(parse16(string,value)&&value==.25,"UTF-16 numeric roundtrip without wchar aliasing");
}
