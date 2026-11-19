"""Build owned Windows x64 diagnostics from a VS developer terminal. No installation."""
import argparse,subprocess
from pathlib import Path
from fetch_sdk import PINS

def main():
 p=argparse.ArgumentParser();p.add_argument('--sdk',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
 a.sdk=a.sdk.resolve();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True);here=Path(__file__).resolve().parent
 for name,(_,commit,_,_) in PINS.items():
  actual=subprocess.check_output(['git','-C',str(a.sdk/name),'rev-parse','HEAD'],text=True).strip()
  if actual!=commit:raise ValueError(f'Wrong {name} revision')
 def compile(name,source,include,dll=False):
  args=['cl','/nologo','/std:c++17','/EHsc','/O2','/MT','/W4','/I'+str(include)]
  if dll:args+=['/LD']
  args+=[str(here/source),'/Fo:'+str(a.out/(name+'.obj')),'/Fe:'+str(a.out/name),'/link','/INCREMENTAL:NO']
  subprocess.run(args,cwd=a.out,check=True)
 compile('ADIEngineProbe.clap','clap/adi_clap_probe.cpp',a.sdk/'clap/include',True)
 compile('clap_selftest.exe','clap/selftest.cpp',a.sdk/'clap/include')
 compile('ScanProbe.vst3','scanner/scan_probe.cpp',a.sdk,True)
 compile('ADIEngineProbe.vst3','vst3/adi_vst3_probe.cpp',a.sdk,True)
 compile('vst3_selftest.exe','vst3/selftest.cpp',a.sdk)
 compile('ADITimeProbe.vst3','time/probe/adi_time_probe.cpp',a.sdk,True)
 compile('time_selftest.exe','time/probe/selftest.cpp',a.sdk)
 print('Built owned diagnostics. Run clap_selftest.exe, vst3_selftest.exe and time_selftest.exe from the output directory; no plug-in was installed.')
if __name__=='__main__':main()
