#!/usr/bin/env python3
"""Check every compiled-in Pd external reaches its signals through FloatSignals -- DEV-09.

    python3 adi_daw/tools/validate_pd_builtins.py

ADI's DSP cores take float. Built with PD_FLOATSIZE=64 (ADI_PD_FLOATSIZE_64,
ADR-0207 d2) Pd's signal vectors are double, and an external that hands
`s_vec` to its core as float* writes half-width garbage into them. Nothing
fails at 32, where t_sample is float, so every pull-request leg is green.

Only the main-only CI leg `pd-double` builds at 64, after the merge. That is
how [adi.oneshot~] reached main red: #649 added it while #658 (FloatSignals,
applied to the 18 externals that existed) was in flight, and the two merged
three hours apart with neither seeing the other. This check is the cheap half
of that leg, run on every pull request through CI's `validate_*.py` glob: a
source file under src/adi/pd_builtins that registers a "dsp" method must
include float_signals.hpp, and no such file may read `s_vec` itself (only
FloatSignals does). It proves the convention, not the numbers -- the 64 leg
still does that.
"""

import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILTINS = os.path.normpath(os.path.join(HERE, '..', 'src', 'adi', 'pd_builtins'))

DSP_METHOD = re.compile(r'gensym\(\s*"dsp"\s*\)')
INCLUDES_FLOAT_SIGNALS = re.compile(r'#\s*include\s*[<"]adi/pd_builtins/float_signals\.hpp[>"]')
READS_S_VEC = re.compile(r'\bs_vec\b')
COMMENTS = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)


def problems_in(text):
    """The rule for one source file; empty when it holds."""
    code = COMMENTS.sub('', text)
    if not DSP_METHOD.search(code):
        return []
    found = []
    if not INCLUDES_FLOAT_SIGNALS.search(code):
        found.append('registers a "dsp" method without including float_signals.hpp')
    if READS_S_VEC.search(code):
        found.append('reads s_vec itself; pass FloatSignals::at(k) to dsp_addv instead')
    return found


def self_test():
    """The rule must catch the shape that broke main, or it proves nothing."""
    broken = ('class_addmethod(k, m, gensym("dsp"), A_CANT, A_NULL);\n'
              't_int a[]{reinterpret_cast<t_int>(s[0]->s_vec)};\n')
    fixed = ('#include "adi/pd_builtins/float_signals.hpp"\n'
             'class_addmethod(k, m, gensym("dsp"), A_CANT, A_NULL);\n'
             '// the signal vectors (s_vec) are double at 64\n'
             't_int a[]{x->signals->at(0)};\n')
    return len(problems_in(broken)) == 2 and not problems_in(fixed)


def main():
    if not self_test():
        print('validate_pd_builtins.py: its own rule no longer catches a raw s_vec external')
        return 1
    sources = sorted(glob.glob(os.path.join(BUILTINS, '*.cpp')))
    if not sources:
        print('no sources under %s -- the path is wrong' % BUILTINS)
        return 1
    bad = []
    checked = 0
    for path in sources:
        with open(path, encoding='utf-8') as f:
            text = f.read()
        if DSP_METHOD.search(COMMENTS.sub('', text)):
            checked += 1
        for p in problems_in(text):
            bad.append('%s: %s' % (os.path.basename(path), p))
    if bad:
        print('Pd externals must take their signals through FloatSignals (DEV-09):')
        print('at PD_FLOATSIZE=64 a raw s_vec is double, and a float core corrupts it.')
        for b in bad:
            print('  ' + b)
        return 1
    print('ok    %d signal externals take their signals through FloatSignals' % checked)
    return 0


if __name__ == '__main__':
    sys.exit(main())
