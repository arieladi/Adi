# SPDX-License-Identifier: GPL-3.0-or-later
# Narrow overlay for libpd 0.16.1 / Pd f009fd8d. Never edit the fetched tree.
function(adi_pd_replace needle replacement)
    string(FIND "${adi_pd_text}" "${needle}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Pinned Pd loader source changed; review the lifetime patch")
    endif()
    string(REPLACE "${needle}" "${replacement}" adi_pd_text "${adi_pd_text}")
    set(adi_pd_text "${adi_pd_text}" PARENT_SCOPE)
endfunction()
set(adi_pd_source "${ADI_THIRD_PARTY}/libpd/pure-data/src")
set(adi_pd_overlay "${CMAKE_CURRENT_BINARY_DIR}/pd-loader-fixed")
file(MAKE_DIRECTORY "${adi_pd_overlay}")
file(READ "${adi_pd_source}/s_loader.c" adi_pd_text)
adi_pd_replace("t_symbol *ll_name;" "char *ll_name;")
adi_pd_replace("    t_symbol *s = gensym(classname);" "    /* Cache keys outlive every Pd instance. Compare owned names. */")
adi_pd_replace("if (ll->ll_name == s)" "if (!strcmp(ll->ll_name, classname))")
adi_pd_replace("    ll->ll_name = gensym(classname);" [=[    ll->ll_name = (char *)getbytes(strlen(classname) + 1);
    strcpy(ll->ll_name, classname);]=])
file(WRITE "${adi_pd_overlay}/s_loader.c" "${adi_pd_text}")
file(READ "${adi_pd_source}/m_class.c" adi_pd_text)
adi_pd_replace("    class_loadsym = s;" [=[    t_symbol *previous_loadsym = class_loadsym;
    class_loadsym = s;]=])
adi_pd_replace("        tryingalready--;" [=[        tryingalready--;
        class_loadsym = previous_loadsym;]=])
adi_pd_replace("    class_loadsym = 0;" "    class_loadsym = previous_loadsym;")
adi_pd_replace("    c->c_name = c->c_helpname = s;" [=[#ifdef PDINSTANCE
    c->c_name = c->c_helpname = s ? dogensym(s->s_name, 0, &pd_maininstance) : 0;
#else
    c->c_name = c->c_helpname = s;
#endif]=])
adi_pd_replace("    class_extern_dir = s;" [=[#ifdef PDINSTANCE
    class_extern_dir = s ? dogensym(s->s_name, 0, &pd_maininstance) : 0;
#else
    class_extern_dir = s;
#endif]=])
adi_pd_replace([=[pd_error(0, "maximum object loading depth %d reached", MAXOBJDEPTH);]=] [=[pd_error(0, "maximum object loading depth %d reached creating '%s' in '%s'", MAXOBJDEPTH, s->s_name, canvas_getcurrentdir()->s_name);]=])
file(WRITE "${adi_pd_overlay}/m_class.c" "${adi_pd_text}")
get_target_property(adi_pd_sources libpd_static SOURCES)
list(FILTER adi_pd_sources EXCLUDE REGEX "(^|/)(m_class|s_loader)\\.c$")
set_property(TARGET libpd_static PROPERTY SOURCES "${adi_pd_sources}")
target_sources(libpd_static PRIVATE "${adi_pd_overlay}/s_loader.c" "${adi_pd_overlay}/m_class.c")
