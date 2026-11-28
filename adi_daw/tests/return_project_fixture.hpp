// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>

namespace adi::test {
// A saved project's authored returns, effects and sends. New does not seed them.
inline std::unique_ptr<Store> createProjectWithReturns(const std::filesystem::path& path, StoreError& error) {
    auto project = Store::createNewProject(path, error);
    if (!project) return nullptr;
    SQLite::Transaction transaction(project->db());
    project->db().exec("UPDATE tracks SET index_in_parent=6 WHERE id=1;"
        "INSERT INTO tracks(id,kind,name,index_in_parent,monitor_mode,color) VALUES"
        "(6,'return','A Reverb',4,0,4),(7,'return','B Delay',5,0,5);"
        "INSERT INTO mixer_strip(track_id) VALUES(6),(7);"
        "INSERT INTO plugin_refs(id,format,uid,vendor,name,version,subtype) VALUES"
        "(1,'internal','org.adidaw.builtin.reverb','ADI','Reverb','1','effect'),"
        "(2,'internal','org.adidaw.builtin.delay','ADI','Delay','1','effect');"
        "INSERT INTO device_chains(id,track_id,name) VALUES(1,6,'Devices'),(2,7,'Devices');"
        "INSERT INTO devices(id,chain_id,plugin_ref_id,name) VALUES(1,1,1,'Reverb'),(2,2,2,'Delay');"
        "INSERT INTO plugin_params(device_id,param_id,name,normalized_value,real_value,display,unit) VALUES"
        "(1,'31','Mix',1.0,100.0,'100 %','%'),(2,'25','Mix',1.0,100.0,'100 %','%');"
        "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,ord,gain_db,pre_fader,enabled) "
        "SELECT (t.id-2)*2+(r.id-5),'track',t.id,'track',r.id,'send',r.id-6,-150.0,0,1 "
        "FROM tracks t CROSS JOIN tracks r WHERE t.id BETWEEN 2 AND 5 AND r.id IN(6,7);");
    transaction.commit();
    return project;
}
} // namespace adi::test
