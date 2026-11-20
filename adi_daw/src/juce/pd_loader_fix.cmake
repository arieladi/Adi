# SPDX-License-Identifier: GPL-3.0-or-later
# Narrow overlay for libpd 0.16.1 / Pd f009fd8d. Never edit the fetched tree.

# DEV-09 (ADR-0207 d2, ON11): the Pd tier built in double precision for the
# 64-bit Processing Precision mode. PD_FLOATSIZE changes t_float, t_sample and
# t_word in m_pd.h, so it is PUBLIC on libpd_static: every translation unit that
# includes Pd (adi_core, the compiled-in externals) sees the same layout.
option(ADI_PD_FLOATSIZE_64 "Build the Pure Data tier with PD_FLOATSIZE=64" OFF)
if(ADI_PD_FLOATSIZE_64)
    target_compile_definitions(libpd_static PUBLIC PD_FLOATSIZE=64)
    message(STATUS "Pd tier: PD_FLOATSIZE=64 (double-precision t_float and t_sample)")
endif()
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
string(REPLACE "\r\n" "\n" adi_pd_text "${adi_pd_text}")
adi_pd_replace("t_symbol *ll_name;" "char *ll_name;")
adi_pd_replace("    t_symbol *s = gensym(classname);" "    /* Cache keys outlive every Pd instance. Compare owned names. */")
adi_pd_replace("if (ll->ll_name == s)" "if (!strcmp(ll->ll_name, classname))")
adi_pd_replace("    ll->ll_name = gensym(classname);" [=[    ll->ll_name = (char *)getbytes(strlen(classname) + 1);
    strcpy(ll->ll_name, classname);]=])
# DEV-09 (ADR-0188 d8, ADR-0203 d8): NATIVE LOADING CLOSED AT THE SOURCE. The
# engine refuses a patch directory or search path holding a loadable file, and
# a [declare -lib]; those guard the door. This removes it: Pd's one native
# loader refuses every library, whatever reached it (an absolute or relative
# object name, a message box, a dynamically created object, -stdlib), and names
# the file on the console. Pd then looks for an abstraction, as after any
# failed load. An external scheduler library is refused too.
adi_pd_replace([=[        /* close dangling filedescriptor */
    close(fd);
]=] [=[        /* close dangling filedescriptor */
    close(fd);
        /* ADI (DEV-09, ADR-0188 d8): no native code from any path. */
    pd_error(0, "ADI refused native code: %s/%s (ADR-0188 d8: externals are compiled in)",
        dirbuf, nameptr);
    (void)objectname; (void)symname; (void)makeout; (void)dlobj;
    return 0;
]=])
adi_pd_replace([=[int sys_run_scheduler(const char *externalschedlibname,
    const char *sys_extraflagsstring)
{]=] [=[int sys_run_scheduler(const char *externalschedlibname,
    const char *sys_extraflagsstring)
{
        /* ADI (DEV-09): no external scheduler library either. */
    if (externalschedlibname)
    {
        pd_error(0, "ADI refused an external scheduler: %s", externalschedlibname);
        return (1);
    }]=])
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
# DEV-09: what a patch may not ask of [; pd ...]. `quit` calls exit() and takes
# ADI with it; `exit` and `verifyquit` stop the scheduler; `open` and `menunew`
# make patches nobody asked for; the path, startup and preference messages
# change where Pd looks for code or write files; the audio and MIDI dialogs
# reconfigure devices Pd does not own here; `fast-forward` runs the scheduler
# for as long as it is told, on whichever thread sent it; `compatibility` sets
# one process-wide level every Pd instance in the session reads. Each becomes one
# refusal naming the message. ADI never sends any of them: it opens patches with
# libpd_openfile (glob_evalfile), not `open`.
file(READ "${adi_pd_source}/m_glob.c" adi_pd_text)
string(REPLACE "\r\n" "\n" adi_pd_text "${adi_pd_text}")
adi_pd_replace([=[void glob_init(void)
{]=] [=[static void adi_glob_refused(t_pd *dummy, t_symbol *s, int argc, t_atom *argv)
{
    (void)dummy; (void)argc; (void)argv;
    pd_error(0, "ADI refused [; pd %s] (DEV-09): a patch cannot quit, open patches, "
        "change paths, devices or preferences", s ? s->s_name : "?");
}

void glob_init(void)
{]=])
foreach(adi_pd_fenced IN ITEMS
        "glob_menunew, gensym(\"menunew\"),\n        A_SYMBOL, A_SYMBOL, 0"
        "glob_open, gensym(\"open\"),\n        A_SYMBOL, A_SYMBOL, A_DEFFLOAT, 0"
        "glob_quit, gensym(\"quit\"), A_DEFFLOAT, 0"
        "glob_exit, gensym(\"exit\"), A_DEFFLOAT, 0"
        "glob_verifyquit,\n        gensym(\"verifyquit\"), A_DEFFLOAT, 0"
        "glob_audio_dialog,\n        gensym(\"audio-dialog\"), A_GIMME, 0"
        "glob_audio_setapi,\n        gensym(\"audio-setapi\"), A_FLOAT, 0"
        "glob_midi_setapi,\n        gensym(\"midi-setapi\"), A_FLOAT, 0"
        "glob_midi_dialog,\n        gensym(\"midi-dialog\"), A_GIMME, 0"
        "glob_path_dialog,\n        gensym(\"path-dialog\"), A_GIMME, 0"
        "glob_addtopath,\n        gensym(\"add-to-path\"), A_SYMBOL, A_DEFFLOAT, 0"
        "glob_startup_dialog,\n        gensym(\"startup-dialog\"), A_GIMME, 0"
        "glob_loadpreferences,\n        gensym(\"load-preferences\"), A_DEFSYM, 0"
        "glob_savepreferences,\n        gensym(\"save-preferences\"), A_DEFSYM, 0"
        "glob_forgetpreferences,\n        gensym(\"forget-preferences\"), A_DEFSYM, 0"
        "glob_fastforward,\n         gensym(\"fast-forward\"), A_FLOAT, 0"
        "glob_compatibility,\n        gensym(\"compatibility\"), A_FLOAT, 0")
    string(REGEX MATCH "gensym\\(\"[a-z-]+\"\\)" adi_pd_name "${adi_pd_fenced}")
    adi_pd_replace("(t_method)${adi_pd_fenced});" "(t_method)adi_glob_refused, ${adi_pd_name}, A_GIMME, 0);")
endforeach()
file(WRITE "${adi_pd_overlay}/m_glob.c" "${adi_pd_text}")
# DEV-09 (PD2, AR18): the #A overlay, from plugdata-pd bdb30eedee ("Correct fix
# for pd bindlist crash (detected by ASAN)"; Pd's own BSD-3 licence). An
# abstraction's canvas binds #A when it finishes loading (canvas_popabstraction)
# and nothing unbinds it; a file load saves and restores #A around itself, but an
# abstraction made at run time (dynamic patching, an attached editor) and then
# freed leaves #A pointing at freed memory, which the next array or text free
# dereferences. pd_free now unbinds whatever it frees from #A first.
file(READ "${adi_pd_source}/m_pd.c" adi_pd_text)
string(REPLACE "\r\n" "\n" adi_pd_text "${adi_pd_text}")
adi_pd_replace([=[void pd_free(t_pd *x)
{
    t_class *c = *x;]=] [=[static t_class *bindlist_class;

void pd_free(t_pd *x)
{
    t_class *c = *x;
        /* ADI (DEV-09; plugdata-pd bdb30eedee): abstraction canvases, [clone]
        and others bind #A while loading and are never explicitly unbound; if
        x is freed while still bound, #A would point at freed memory. */
    {
        t_symbol *a = gensym("#A");
        if (a->s_thing && (a->s_thing == x || *a->s_thing == bindlist_class))
            pd_unbind(x, a);
    }]=])
file(WRITE "${adi_pd_overlay}/m_pd.c" "${adi_pd_text}")
# DF-E36 (a), (b): a patch cannot close the patch ADI opened, nor quit. In
# canvas_menuclose, force 0 or 1 to a top-level canvas and force 2 to any
# canvas free the root under the engine (a use after free and a double free
# in LibPdEngine::close); force 3 calls glob_verifyquit directly, past the
# glob fence, and with no dirty canvas sets the process-wide sys_quit that
# libpd never clears -- every instance's scheduler then skips its DSP. Each
# is refused by name. Force 0 or 1 to a subpatch, which headless only hides
# it, stays.
file(READ "${adi_pd_source}/g_editor.c" adi_pd_text)
string(REPLACE "\r\n" "\n" adi_pd_text "${adi_pd_text}")
adi_pd_replace([=[void canvas_menuclose(t_canvas *x, t_floatarg fforce)
{
    int force = fforce;]=] [=[void canvas_menuclose(t_canvas *x, t_floatarg fforce)
{
    int force = fforce;
    if (force == 3 || force == 2 || !x->gl_owner)
    {
        pd_error(0, "ADI refused [menuclose %d] (DEV-09): a patch cannot close the patch "
            "it is in, or quit", force);
        return;
    }]=])
file(WRITE "${adi_pd_overlay}/g_editor.c" "${adi_pd_text}")
# DF-E36 (c): no help patch is opened for a patch. The canvas's and [array
# define]'s `done-popup 2` reach glob_evalfile through open_via_helppath: the
# same effect the glob fence refuses for `open` and `menunew`. ADI calls it
# from nowhere; libpd_openfile and abstraction loading do not pass here.
file(READ "${adi_pd_source}/s_path.c" adi_pd_text)
string(REPLACE "\r\n" "\n" adi_pd_text "${adi_pd_text}")
adi_pd_replace([=[void open_via_helppath(const char *name, const char *dir)
{]=] [=[void open_via_helppath(const char *name, const char *dir)
{
    if (name)
    {
        pd_error(0, "ADI refused a help patch: %s (DEV-09)", name);
        return;
    }]=])
file(WRITE "${adi_pd_overlay}/s_path.c" "${adi_pd_text}")
# DF-E36 (e): a patch cannot change where Pd looks for code. canvas_declare
# serves the [declare] box, the file's `#X declare` record and a run-time
# `declare` message alike; its -path, -stdpath, -lib and -stdlib are refused
# by name. The engine's own libpd_add_to_search_path is untouched.
file(READ "${adi_pd_source}/g_canvas.c" adi_pd_text)
string(REPLACE "\r\n" "\n" adi_pd_text "${adi_pd_text}")
foreach(adi_pd_call IN ITEMS "canvas_path(x, e, item);" "canvas_stdpath(e, item);"
                             "canvas_lib(x, e, item);" "canvas_stdlib(e, item);")
    adi_pd_replace("            ${adi_pd_call}"
        "            pd_error(0, \"ADI refused [declare %s %s] (DEV-09): a patch cannot change where Pd looks for code\", flag, item);")
endforeach()
file(WRITE "${adi_pd_overlay}/g_canvas.c" "${adi_pd_text}")
# DF-E36 (a)'s "menusave and menusaveas, which write files": a patch cannot
# write files through its canvas. `savetofile` writes the patch's text to any
# name in any folder, `menusave` overwrites the patch's own file with what the
# patch made of itself at run time, `write` writes the canvas's data; each is
# a message any patch can send, past the PD4 walk, and each is refused by
# name. ADI saves no patch through Pd. menusaveas only asks a GUI (none here).
file(READ "${adi_pd_source}/g_readwrite.c" adi_pd_text)
string(REPLACE "\r\n" "\n" adi_pd_text "${adi_pd_text}")
adi_pd_replace([=[static void canvas_savetofile(t_canvas *x, t_symbol *filename, t_symbol *dir,
    float fdestroy)
{]=] [=[static void canvas_savetofile(t_canvas *x, t_symbol *filename, t_symbol *dir,
    float fdestroy)
{
    if (filename)
    {
        pd_error(0, "ADI refused [savetofile %s] (DEV-09): a patch cannot write files",
            filename->s_name);
        return;
    }]=])
adi_pd_replace([=[static void canvas_menusave(t_canvas *x, t_float fdestroy)
{]=] [=[static void canvas_menusave(t_canvas *x, t_float fdestroy)
{
    if (x)
    {
        pd_error(0, "ADI refused [menusave] (DEV-09): a patch cannot write files");
        return;
    }]=])
adi_pd_replace([=[static void glist_write(t_glist *x, t_symbol *filename, t_symbol *format)
{]=] [=[static void glist_write(t_glist *x, t_symbol *filename, t_symbol *format)
{
    if (filename)
    {
        pd_error(0, "ADI refused [write %s] (DEV-09): a patch cannot write files",
            filename->s_name);
        return;
    }]=])
file(WRITE "${adi_pd_overlay}/g_readwrite.c" "${adi_pd_text}")
get_target_property(adi_pd_sources libpd_static SOURCES)
list(FILTER adi_pd_sources EXCLUDE REGEX "(^|/)(m_class|s_loader|g_io|m_glob|m_pd|g_editor|s_path|g_canvas|g_readwrite)\\.c$")
set_property(TARGET libpd_static PROPERTY SOURCES "${adi_pd_sources}")
target_sources(libpd_static PRIVATE "${adi_pd_overlay}/s_loader.c" "${adi_pd_overlay}/m_class.c"
                                    "${adi_pd_overlay}/g_io.c" "${adi_pd_overlay}/m_glob.c"
                                    "${adi_pd_overlay}/m_pd.c" "${adi_pd_overlay}/g_editor.c"
                                    "${adi_pd_overlay}/s_path.c" "${adi_pd_overlay}/g_canvas.c"
                                    "${adi_pd_overlay}/g_readwrite.c")
