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
# Multi-line needles below are written with LF: a checkout with CRLF must match.
string(REPLACE "\r\n" "\n" adi_pd_text "${adi_pd_text}")
# DEV-09: a freed instance must free what its creation allocated. Measured on
# Pd f009fd8d under the MSVC debug heap, every libpd_free_instance left 17
# blocks: the t_pdinstance itself, the names of its twelve built-in symbols
# (their t_symbol structs are embedded, so the hash walk skips them whole),
# its two DAC/ADC buffers and its search-path list.
adi_pd_replace([=[                freebytes((void *)s->s_name, strlen(s->s_name)+1);
                freebytes(s, sizeof(*s));
            }]=] [=[                freebytes((void *)s->s_name, strlen(s->s_name)+1);
                freebytes(s, sizeof(*s));
            }
            else freebytes((void *)s->s_name, strlen(s->s_name)+1);]=])
adi_pd_replace([=[    s_inter_free(inter);  /* must happen after sys_unlock() */
}]=] [=[    s_inter_free(inter);  /* must happen after sys_unlock() */
    freebytes(x, sizeof(*x));
}]=])
adi_pd_replace([=[void s_stuff_freepdinstance(void)
{
    freebytes(STUFF, sizeof(*STUFF));]=] [=[void s_stuff_freepdinstance(void)
{
    namelist_free(STUFF->st_externlist);
    namelist_free(STUFF->st_searchpath);
    namelist_free(STUFF->st_staticpath);
    namelist_free(STUFF->st_helppath);
    namelist_free(STUFF->st_temppath);
    if (STUFF->st_soundin) freebytes(STUFF->st_soundin, 0);
    if (STUFF->st_soundout) freebytes(STUFF->st_soundout, 0);
    freebytes(STUFF, sizeof(*STUFF));]=])
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
# DEV-09: [inlet~] and [outlet~] free each channel's buffer but never the
# array of reblockers itself (one t_reblocker per channel, from getbytes or
# reblocker_resize), so every signal inlet or outlet of every patch or
# abstraction closed leaked it.
file(READ "${adi_pd_source}/g_io.c" adi_pd_text)
string(REPLACE "\r\n" "\n" adi_pd_text "${adi_pd_text}")
foreach(adi_pd_io IN ITEMS vinlet voutlet)
    if(adi_pd_io STREQUAL "vinlet")
        set(adi_pd_rm "canvas_rminlet(x->x_canvas, x->x_inlet);")
    else()
        set(adi_pd_rm "canvas_rmoutlet(x->x_canvas, x->x_parentoutlet);")
    endif()
    adi_pd_replace("static void ${adi_pd_io}_free(t_${adi_pd_io} *x)
{
    int i;
    ${adi_pd_rm}
    if (x->x_rb)
        for (i = 0; i < x->x_nchans; i++)
    {
        freebytes((x->x_rb)[i].r_buf, x->x_buflength * sizeof(t_sample));
        resample_free(&(x->x_rb)[i].r_updown);
    }
}" "static void ${adi_pd_io}_free(t_${adi_pd_io} *x)
{
    int i;
    ${adi_pd_rm}
    if (x->x_rb)
    {
        for (i = 0; i < x->x_nchans; i++)
        {
            freebytes((x->x_rb)[i].r_buf, x->x_buflength * sizeof(t_sample));
            resample_free(&(x->x_rb)[i].r_updown);
        }
        freebytes(x->x_rb, x->x_nchans * sizeof(*x->x_rb));
        x->x_rb = 0;
    }
}")
endforeach()
file(WRITE "${adi_pd_overlay}/g_io.c" "${adi_pd_text}")
get_target_property(adi_pd_sources libpd_static SOURCES)
list(FILTER adi_pd_sources EXCLUDE REGEX "(^|/)(m_class|s_loader|g_io)\\.c$")
set_property(TARGET libpd_static PROPERTY SOURCES "${adi_pd_sources}")
target_sources(libpd_static PRIVATE "${adi_pd_overlay}/s_loader.c" "${adi_pd_overlay}/m_class.c"
                                    "${adi_pd_overlay}/g_io.c")
