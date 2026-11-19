# PD-13 / ADR-0024: SQLite 3.53.4 recovery sources, public domain.
# Official Git mirror b09c88c14082339b66c7b7158d609a771e64ca69;
# Fossil release bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc.
set(_adi_recover "${ADI_THIRD_PARTY}/sqlite-recover/ext/recover")
foreach(_adi_file_hash IN ITEMS
    "sqlite3recover.c:792ba92b381921098fdc637b2f1e97dee06f505b06d29c832b48160ed58c4339"
    "sqlite3recover.h:c088088d15af055304e931842f7c197024d46ff1b7077912ec42149e9fa4f9bb"
    "dbdata.c:0a57eae22b51507c28f6c647fba00ccacd39b5aaf3393dfe9435fb04810a6e0e")
    string(REPLACE ":" ";" _adi_pair "${_adi_file_hash}")
    list(GET _adi_pair 0 _adi_name)
    list(GET _adi_pair 1 _adi_hash)
    if(NOT EXISTS "${_adi_recover}/${_adi_name}")
        message(FATAL_ERROR "Missing pinned sqlite-recover; run tools/fetch_external.sh --build-only")
    endif()
    file(READ "${_adi_recover}/${_adi_name}" _adi_content)
    string(REPLACE "\r\n" "\n" _adi_content "${_adi_content}")
    string(SHA256 _adi_actual "${_adi_content}")
    if(NOT _adi_actual STREQUAL _adi_hash)
        message(FATAL_ERROR "sqlite-recover/${_adi_name} does not match the pinned SQLite 3.53.4 source")
    endif()
endforeach()
file(STRINGS "${ADI_THIRD_PARTY}/SQLiteCpp/sqlite3/sqlite3.h" _adi_sqlite_source REGEX "^#define SQLITE_SOURCE_ID ")
if(NOT _adi_sqlite_source MATCHES "2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc")
    message(FATAL_ERROR "SQLiteCpp's SQLite source ID must match the pinned recovery extension")
endif()
# The upstream recovery writer requires writable_schema to reconstruct
# sqlite_sequence before AUTOINCREMENT tables. With DEFAULT_DEFENSIVE the
# pragma is ignored; upstream then maps an AUTOINCREMENT table's root to the
# implicitly created sqlite_sequence and silently loses intact rows. Disable
# DEFENSIVE on that dedicated private output connection only, not the input,
# ordinary connections, or the global default. Keep the checkout unmodified.
file(READ "${_adi_recover}/sqlite3recover.c" _adi_recover_source)
string(REPLACE "\r\n" "\n" _adi_recover_source "${_adi_recover_source}")
set(_adi_recover_anchor [=[  if( sqlite3_open_v2(p->zUri, &db, flags, 0) ){
    recoverDbError(p, db);
  }
]=])
string(FIND "${_adi_recover_source}" "${_adi_recover_anchor}" _adi_anchor_at)
if(_adi_anchor_at EQUAL -1)
    message(FATAL_ERROR "Pinned recovery output-open anchor is absent")
endif()
set(_adi_recover_patch [=[
  /* ADI_PD13_PRIVATE_RECOVERY_OUTPUT: only this isolated staging writer
  ** needs writable_schema; all ordinary connections remain defensive. */
  if( p->errCode==SQLITE_OK ){
    p->errCode = sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 0, 0);
  }
]=])
string(REPLACE "${_adi_recover_anchor}" "${_adi_recover_anchor}${_adi_recover_patch}"
    _adi_recover_source "${_adi_recover_source}")
string(REGEX MATCHALL "ADI_PD13_PRIVATE_RECOVERY_OUTPUT" _adi_patch_matches "${_adi_recover_source}")
list(LENGTH _adi_patch_matches _adi_patch_count)
if(NOT _adi_patch_count EQUAL 1)
    message(FATAL_ERROR "Recovery output configuration patch must apply exactly once")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sqlite-recover-adapted")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sqlite-recover-adapted/sqlite3recover.c"
    CONTENT "${_adi_recover_source}" @ONLY NEWLINE_STYLE LF)
string(SHA256 _adi_adapted_hash "${_adi_recover_source}")
message(STATUS "PD-13 private-output recovery adapter SHA-256: ${_adi_adapted_hash}")
add_library(adi_sqlite_recover STATIC "${CMAKE_CURRENT_BINARY_DIR}/sqlite-recover-adapted/sqlite3recover.c" "${_adi_recover}/dbdata.c")
target_include_directories(adi_sqlite_recover SYSTEM PUBLIC "${_adi_recover}")
target_link_libraries(adi_sqlite_recover PUBLIC sqlite3)
