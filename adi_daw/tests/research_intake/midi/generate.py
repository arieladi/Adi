"""Generate synthetic SMF research inputs. Does not connect to or run a DAW."""
from pathlib import Path
import struct,hashlib,json,argparse
parser=argparse.ArgumentParser();parser.add_argument('--out',type=Path,required=True);P=parser.parse_args().out;P.mkdir(parents=True,exist_ok=True)
def vlq(n):
 assert n>=0
 a=[n&127];n>>=7
 while n:a.insert(0,(n&127)|128);n>>=7
 return bytes(a)
def note(t,p,d=480,v=80,ch=0,off=64):return [(t,bytes([0x90+ch,p,v])),(t+d,bytes([0x80+ch,p,off]))]
fixtures={
 'ADI_TEST_chord':sum((note(0,p,960,40+i*25,off=20+i*20) for i,p in enumerate([60,64,67])),[]),
 'ADI_TEST_overlap_same_key':note(0,60,960)+note(480,60,960,100),
 'ADI_TEST_irregular_timing':sum((note(t,60+i,170+i*31,45+i*9) for i,t in enumerate([0,119,481,727,1111,1679])),[]),
 'ADI_TEST_boundary_notes':note(0,55,600)+note(420,60,240)+note(900,64,600)+note(1920,67,480),
 'ADI_TEST_scale_outliers':sum((note(i*240,p,180) for i,p in enumerate([60,61,62,63,64,66,67,70])),[]),
 'ADI_TEST_release_velocities':sum((note(i*480,60,360,80,off=v) for i,v in enumerate([0,32,64,127])),[]),
 'ADI_TEST_velocity_extremes':sum((note(i*480,60,360,v) for i,v in enumerate([1,32,96,127])),[]),
 'ADI_TEST_per_channel_expression':note(0,60,1440,80,1)+note(240,67,1440,90,2)+[(300,bytes([0xE1,0,72])),(420,bytes([0xE2,0,56])),(600,bytes([0xD1,90])),(720,bytes([0xD2,30])),(840,bytes([0xB1,74,100])),(960,bytes([0xB2,74,20])),(1800,bytes([0xE1,0,64])),(1800,bytes([0xE2,0,64]))]
}
manifest=[]
for name,events in fixtures.items():
 events=sorted(events,key=lambda e:e[0])
 track=b'\0\xff\x51\x03\x07\xa1\x20'+b'\0\xff\x58\x04\x04\x02\x18\x08';last=0
 for t,event in events:track+=vlq(t-last)+event;last=t
 track+=vlq(max(0,3840-last))+b'\xff\x2f\0'
 data=b'MThd'+struct.pack('>IHHH',6,0,1,480)+b'MTrk'+struct.pack('>I',len(track))+track
 path=P/(name+'.mid');path.write_bytes(data)
 manifest.append(dict(file=path.name,sha256=hashlib.sha256(data).hexdigest(),format=0,ppq=480,tempo_bpm=120,end_tick=3840,events=[dict(tick=t,hex=e.hex()) for t,e in events]))
(P/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf8')
(P/'README.md').write_text('''# Synthetic MIDI research inputs

Eight format-0 SMF files, PPQ 480, 120 BPM, 4/4, two bars. They are inputs, not captured DAW output. Use a fresh clip per test and record import settings. The manifest includes every MIDI event and its absolute tick. Same-key overlap may intentionally be simplified by import; record that before testing an edit.

The per-channel-expression file contains independent pitch bend, channel pressure and CC74 on channels 2 and 3. It does not send MPE zone configuration or pitch-bend sensitivity RPNs. Configure and record those explicitly; this file alone does not establish that a host imported MPE or preserved note identity. For definitive expression tests, draw separate expressions on two simultaneous notes in the host and save a baseline too.

Expected fixture time positions are independent of project tempo. Do not confuse PPQ ticks, host beats and sample positions. No real instrument is necessary to inspect note edits; sound tests can use a documented neutral test instrument in an isolated test harness.
''',encoding='utf8')
print('Generated',len(manifest),'synthetic MIDI files')
