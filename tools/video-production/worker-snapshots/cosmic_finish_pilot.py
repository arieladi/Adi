from pathlib import Path
import os,sys,json,time,subprocess,traceback
os.environ['CUDA_VISIBLE_DEVICES']='GPU-5e287f99-46c5-350a-0c85-50e9588f1795'
import cv2,numpy as np,torch,imageio_ffmpeg
from scipy.interpolate import PchipInterpolator
R=Path(__file__).parent;P=json.loads((R/'plan.json').read_text());FF=imageio_ffmpeg.get_ffmpeg_exe()
sys.path.insert(0,'D:/AvasthaAI/EnhancerV2')
from models import Interpolator
def save(p,v):
 q=p.with_suffix('.tmp');q.write_text(json.dumps(v,indent=2),encoding='utf-8');q.replace(p)
def status(stage,**kw):save(R/'finish-status.json',dict(stage=stage,time=time.time(),**kw))
def read(p):return cv2.cvtColor(cv2.imread(str(p)),cv2.COLOR_BGR2RGB)[8:728].copy()
def encoder(out):
 return subprocess.Popen([FF,'-v','error','-y','-f','rawvideo','-pix_fmt','rgb24','-s','1280x720','-r','24','-i','pipe:0','-an','-c:v','libx264','-crf','15','-preset','fast','-threads','3','-pix_fmt','yuv420p',str(out)],stdin=subprocess.PIPE)
def main():
 with (R/'finish.lock').open('x') as f:f.write(str(os.getpid()))
 try:
  torch.set_num_threads(2);rife=None
  for s in P['shots']:
   d=R/'shots'/f"{s['index']:02d}";d.mkdir(parents=True,exist_ok=True)
   if (d/'finished.json').exists():continue
   status('waiting-for-shot',shot=s['index']+1)
   while not (d/'generated.json').exists():
    if (R/'generation-status.json').exists() and json.loads((R/'generation-status.json').read_text()).get('stage')=='failed':raise RuntimeError('Generation failed; inspect existing receipts')
    time.sleep(20)
   if rife is None:rife=Interpolator(Path('D:/AvasthaAI/Tools/RIFE426'),torch.device('cuda:0'),False,False,1.0)
   g=json.loads((d/'generated.json').read_text());paths=sorted(Path(g['raw']).glob('*.png'));assert len(paths)==121
   thumbs=np.array([cv2.resize(read(p),(160,90)).astype(np.float32) for p in paths]);changes=np.abs(np.diff(thumbs,axis=0)).mean(axis=(1,2,3))
   peak=int(np.argmax(changes[30:90])+31);n=s['end_frame']-s['start_frame']
   mid=(s['start']+s['end'])/2;beat=P['phase']+round((mid-P['phase'])*143/60)*60/143
   accent=max(1,min(n-2,round(beat*24)-s['start_frame']))
   # Only a measured motion-change landmark; semantic action completion still requires review.
   positions=PchipInterpolator([0,accent,n-1],[0,peak,120])(np.arange(n)) if s['active'] else np.linspace(0,120,n)
   save(d/'timing.json',{'positions':positions.tolist(),'raw_motion_peak':peak,'target_accent_frame':accent,'semantic_completion_verified':False})
   status('retiming',shot=s['index']+1,frames=n);out=d/'retimed.mp4';enc=encoder(out);target=torch.empty((720,1280,3),dtype=torch.uint8);cached=-1
   try:
    for pos in positions:
     lo=min(120,int(pos));frac=float(pos-lo)
     if lo!=cached:
      left=read(paths[lo]);right=read(paths[min(120,lo+1)]);lg=rife.upload(torch.from_numpy(left));rg=rife.upload(torch.from_numpy(right));cached=lo
     if frac<1e-6 or lo==120:frame=left
     else:rife.render(lg,rg,frac,target);frame=target.numpy()
     enc.stdin.write(frame.tobytes())
    enc.stdin.close();assert enc.wait()==0
   finally:
    if enc.poll() is None:enc.kill()
   cap=cv2.VideoCapture(str(out));count=0
   while cap.read()[0]:count+=1
   cap.release();assert count==n
   save(d/'finished.json',{'frames':n,'path':str(out),'rife':'4.26 FP32 RTX4000','color_grading':False})
  status('assembling')
  # Read the original opening, preserving its first192 frames at24fps.
  opening=Path('D:/AvasthaAI/CosmicRain-20261025/inputs/opening.mp4');op=R/'opening-8s.mp4'
  subprocess.run([FF,'-v','error','-y','-i',str(opening),'-t','8','-vf','scale=1280:720:force_original_aspect_ratio=increase,crop=1280:720,setsar=1,fps=24','-an','-c:v','libx264','-crf','15','-threads','3',str(op)],check=True)
  silent=R/'pilot-silent.mp4';enc=encoder(silent);previous=[];total=0
  yy,xx=np.mgrid[:720,:1280].astype(np.float32);radius=np.sqrt(((xx-640)/735)**2+((yy-360)/735)**2)
  items=[(op,None)]+[(R/'shots'/f"{s['index']:02d}"/'retimed.mp4',s) for s in P['shots']]
  for path,s in items:
   cap=cv2.VideoCapture(str(path));j=0;recent=[]
   while True:
    ok,f=cap.read()
    if not ok:break
    f=cv2.cvtColor(f,cv2.COLOR_BGR2RGB)
    if s and s['join']=='portal' and j<6 and previous:
     a=((j+1)/6)*(1+1/12);mask=np.clip((a-radius)*12,0,1)[...,None];f=np.rint(f*mask+previous[-1]*(1-mask)).astype(np.uint8)
    if s and s['symmetry']:
     n=s['end_frame']-s['start_frame'];center=n//2;distance=abs(j-center)
     if distance<6:
      amount=.65*np.cos(distance*np.pi/12)**2;mir=f.copy();mir[:,640:]=f[:,:640][:,::-1];f=np.rint(f*(1-amount)+mir*amount).astype(np.uint8)
    enc.stdin.write(f.tobytes());recent=[f];j+=1;total+=1
   cap.release();previous=recent
  enc.stdin.close();assert enc.wait()==0;assert total==2880,total
  out=R/'Cosmic Rain - DRAFT first 120 seconds - musical edit - with music.mp4'
  subprocess.run([FF,'-v','error','-y','-i',str(silent),'-i','D:/AvasthaAI/CosmicRain-20261025/inputs/track.wav','-map','0:v','-map','1:a','-c:v','copy','-c:a','aac','-b:a','320k','-t','120','-movflags','+faststart',str(out)],check=True)
  subprocess.run([FF,'-v','error','-i',str(out),'-f','null','-'],check=True)
  save(R/'final-receipt.json',{'output':str(out),'frames':2880,'fps':24,'size':[1280,720],'duration':120,'full_decode':True,'music_original_speed':True,'pending_user_review':True,'first8seconds':'Original opening reencoded to delivery settings','timing':'Motion landmarks, no semantic beat guarantee'})
  status('complete',output=str(out))
 finally:(R/'finish.lock').unlink(missing_ok=True)
if __name__=='__main__':
 try:main()
 except Exception:status('failed',error=traceback.format_exc());raise
