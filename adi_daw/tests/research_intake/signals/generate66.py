"""Deterministic research stimuli and explicit-alignment residual analysis.
Does not launch a DAW. Generated fixtures and self-checks are not DAW measurements.
"""
from pathlib import Path
import argparse, hashlib, json
import numpy as np
from scipy.io import wavfile



def make(folder):
 folder=Path(folder);folder.mkdir(parents=True,exist_ok=True);manifest=[]
 for rate in (44100,48000,96000):
  n=rate*4;t=np.arange(n)/rate;rng=np.random.default_rng(20261104)
  impulse=np.zeros(n);impulse[rate]=0.5
  step=np.zeros(n);step[rate:rate*2]=0.25
  sweep=np.sin(2*np.pi*20*4/np.log(900)*(np.exp(t*np.log(900)/4)-1))*0.125
  noise=rng.uniform(-0.125,0.125,n)
  signals={'silence':np.zeros(n),'impulse':impulse,'step':step,'dc':np.ones(n)*0.125,
    'sine_997':np.sin(2*np.pi*997*t)*0.25,'sweep_20_18000':sweep,
    'multitone':sum(np.sin(2*np.pi*f*t) for f in (71,223,997,3001,7001,11003))/32,
    'noise':noise,'precision_A':np.ones(n)*0.5,'precision_B':np.ones(n)*2**-26,
    'precision_minus_A':np.ones(n)*-0.5}
  for name,sig in signals.items():
   for channels in (1,2):
    x=sig if channels==1 else np.column_stack([sig,sig*(-0.5)])
    p=folder/f'{name}_{rate}_{channels}ch_f32.wav';wavfile.write(p,rate,x.astype(np.float32))
    manifest.append(dict(path=p.name,rate=rate,channels=channels,frames=n,sha256=hashlib.sha256(p.read_bytes()).hexdigest()))
 (folder/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf8')
 print('Generated',len(manifest),'float32 WAV probes. No DAW was executed.')

def compare(reference,candidate,offset,output):
 sr,x=wavfile.read(reference);st,y=wavfile.read(candidate)
 if sr!=st:raise ValueError('Sample-rate mismatch; resampling is never implicit')
 def floating(z):
  if z.dtype.kind=='i':z=z.astype(np.float64)/2**(z.dtype.itemsize*8-1)
  elif z.dtype==np.uint8:z=(z.astype(np.float64)-128)/128
  else:z=z.astype(np.float64)
  return z[:,None] if z.ndim==1 else z
 x=floating(x);y=floating(y)
 if x.shape[1]!=y.shape[1]:raise ValueError('Channel-count mismatch')
 if not np.isfinite(x).all() or not np.isfinite(y).all():raise ValueError('Non-finite audio')
 original_lengths=[len(x),len(y)]
 if offset>=0:y=y[offset:]
 else:x=x[-offset:]
 n=min(len(x),len(y))
 if not n:raise ValueError('No overlapping frames')
 e=y[:n]-x[:n]
 rms=np.sqrt(np.mean(e*e,axis=0));peak=np.max(np.abs(e),axis=0)
 energy=np.sum(x[:n]**2,axis=0)
 gain=np.divide(np.sum(x[:n]*y[:n],axis=0),energy,out=np.full(x.shape[1],np.nan),where=energy>0)
 window=np.hanning(n);spectrum=np.abs(np.fft.rfft(e*window[:,None],axis=0))
 out=Path(output);out.parent.mkdir(parents=True,exist_ok=True)
 np.savez_compressed(out.with_suffix('.spectrum.npz'),frequency=np.fft.rfftfreq(n,1/sr),residual_magnitude=spectrum)
 report=dict(reference=str(reference),candidate=str(candidate),reference_sha256=hashlib.sha256(Path(reference).read_bytes()).hexdigest(),candidate_sha256=hashlib.sha256(Path(candidate).read_bytes()).hexdigest(),sample_rate=sr,channels=x.shape[1],original_frames=original_lengths,explicit_candidate_delay_frames=offset,compared_frames=n,uncompared_frames=[len(x)-n,len(y)-n],exact_equal_in_compared_region=bool(np.array_equal(x[:n],y[:n])),residual_peak=peak.tolist(),residual_rms=rms.tolist(),least_squares_gain=[float(v) if np.isfinite(v) else None for v in gain],normalization='none',spectrum_window='Hann; unnormalized FFT magnitude',interpretation='No perceptual equivalence inferred. Uncompared prefix/tail and unequal lengths require separate inspection.')
 out.write_text(json.dumps(report,indent=2,allow_nan=False),encoding='utf8');print(json.dumps(report,indent=2))

if __name__=='__main__':
 p=argparse.ArgumentParser();sub=p.add_subparsers(dest='command',required=True);sub.add_parser('make').add_argument('--out',required=True)
 c=sub.add_parser('compare');c.add_argument('reference');c.add_argument('candidate');c.add_argument('--offset',type=int,default=0);c.add_argument('--output',required=True)
 a=p.parse_args()
 if a.command=='make':make(a.out)
 else:compare(a.reference,a.candidate,a.offset,a.output)
