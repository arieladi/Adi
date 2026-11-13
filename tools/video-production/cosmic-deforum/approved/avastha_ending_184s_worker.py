from pathlib import Path
import json,os,time,urllib.request,subprocess,hashlib,shutil,traceback
R=Path('D:/AvasthaAI/DeforumEnding184-20261023'); R.mkdir(exist_ok=True)
BASE='http://127.0.0.1:7865'; N=2760

def save(name,obj):
 p=R/name; t=p.with_suffix(p.suffix+'.tmp');t.write_text(json.dumps(obj,indent=2),encoding='utf-8');t.replace(p)
def status(stage,**kw): save('status.json',dict(stage=stage,updated=time.time(),**kw))
def api(path,data=None,timeout=60):
 req=urllib.request.Request(BASE+path,data=None if data is None else json.dumps(data).encode(),headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(req,timeout=timeout) as f:return json.load(f)
def digest(p):
 with open(p,'rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def main():
 if (R/'submission-receipt.json').exists():
  receipt=json.loads((R/'submission-receipt.json').read_text())
 else:
  if (R/'submission-marker.json').exists():raise RuntimeError('Submission outcome uncertain; inspect API before retrying')
  settings=json.loads(Path('D:/AvasthaAI/ActualDeforum/settings-8s.json').read_text())
  settings.update(max_frames=N,batch_name='Avastha_SD15_Ending184_20261023',init_image=str(R/'start.png'))
  # Same approved camera, sampler, noise and per-frame settings. Longer evolving themes.
  themes=[
   'Luminous crystalline branching tunnel, pale violet trees forming circular arches above a mirrored river, intricate golden roots, deep layered visionary psychedelic landscape, coherent detailed forms',
   'The crystalline tree arches unfurl into enormous bioluminescent mushroom gills, translucent violet petals, curling turquoise ferns, intricate organic gold filaments, layered alien garden',
   'The mushroom forest grows into emerald living cathedral chambers, coral branches and luminous lotus flowers, deep layered psychedelic organic architecture',
   'Emerald cathedral branches transform into underwater coral spirals, translucent jellyfish bells and flowing blue anemones, luminous golden filaments, intricate deep ocean world',
   'The ocean coral opens into immense cosmic shells, spiraling purple nebulas and crystalline planets, intricate iridescent chambers, layered visionary space landscape',
   'Cosmic shells unfold into golden mandala tunnels, luminous star rivers and galaxies woven into branching roots, intricate fractal celestial architecture',
   'Golden mandala roots form a monumental serene Buddha face carved from translucent living crystal, emerald forests growing through the crown, violet lotus petals',
   'The crystal Buddha dissolves organically into an immense luminous sacred tree, branching golden roots and turquoise leaves forming deep intricate fractal forest chambers',
   'Sacred tree leaves unfold into translucent butterflies and violet mushroom gardens, luminous organic geometric spirals, intricate emerald and golden alien nature',
   'The alien garden blooms into immense iridescent lotus petals enclosing a cosmic ocean, glowing crystalline roots and spiral shells, deep visionary psychedelic landscape',
   'The lotus ocean expands into a luminous golden cosmic tree, emerald branches woven around violet galaxies, intricate peaceful celestial sanctuary, rich coherent detail'
  ]
  settings['prompts']={str(i*270):t+', visionary psychedelic painting, rich intricate detail, continuous organic transformation' for i,t in enumerate(themes)}
  # Repeat the approved strength envelope without any beat-synced motion.
  keys={}
  for base in range(0,N,120):
   for offset,value in [(0,.62),(35,.55),(60,.48),(90,.55),(119,.60)]:
    if base+offset<N:keys[base+offset]=value
  settings['strength_schedule']=', '.join(f'{k}:({v})' for k,v in sorted(keys.items()))
  save('settings.json',settings)
  api('/sdapi/v1/reload-checkpoint',{},timeout=180)
  save('submission-marker.json',{'time':time.time(),'batch':settings['batch_name']})
  receipt=api('/deforum_api/batches',{'deforum_settings':settings,'options_overrides':{'img2img_fix_steps':True}},timeout=180)
  save('submission-receipt.json',receipt)
 job_id=receipt['job_ids'][0]
 status('generating',job_id=job_id,raw_frames=0,total_frames=N)
 # Color the generated frames as they arrive. Heavy stages stay isolated from A2000.
 os.environ['CUDA_VISIBLE_DEVICES']='GPU-5e287f99-46c5-350a-0c85-50e9588f1795'
 import numpy as np,torch
 import torch.nn.functional as F
 from PIL import Image,ImageFilter
 import imageio_ffmpeg,av
 lut=np.load(R/'approved-lut.npy').astype(np.float32)
 volume=torch.from_numpy(lut.reshape(16,16,16,3).transpose(3,0,1,2).copy()).unsqueeze(0).cuda()
 @torch.inference_mode()
 def grade(a):
  grid=torch.from_numpy(a.copy()).cuda().float().div_(127.5).sub_(1).unsqueeze(0).unsqueeze(0)
  return F.grid_sample(volume,grid,mode='bilinear',padding_mode='border',align_corners=True)[0,:,0].permute(1,2,0).mul(255).round().clamp(0,255).byte().cpu().numpy()
 # Verify GPU LUT interpolation against the approved PIL implementation.
 sample=np.asarray(Image.open(R/'start.png').convert('RGB').resize((256,144)))
 reference=np.asarray(Image.fromarray(sample).filter(ImageFilter.Color3DLUT(16,lut.flatten().tolist())))
 delta=np.abs(grade(sample).astype(float)-reference.astype(float))
 if delta.max()>3 or delta.mean()>1:raise RuntimeError(f'GPU LUT validation failed {delta.mean()} / {delta.max()}')
 save('color-receipt.json',{'lut_sha256':digest(R/'approved-lut.npy'),'device':torch.cuda.get_device_name(0),'max_difference_from_approved_PIL':float(delta.max()),'mean_difference':float(delta.mean()),'policy':'Only newly generated Deforum frames graded; cloud source untouched'})
 ff=imageio_ffmpeg.get_ffmpeg_exe(); raw=R/'graded-184s.mkv'
 process=None; count=0; began=time.time();outdir=None;prefix=None
 try:
  with (R/'color-encode.log').open('wb') as log:
   process=subprocess.Popen([ff,'-hide_banner','-loglevel','error','-n','-f','rawvideo','-pix_fmt','rgb24','-s','848x480','-r','15','-i','pipe:0','-an','-c:v','ffv1','-level','3','-threads','2',str(raw)],stdin=subprocess.PIPE,stderr=log)
   while count<N:
    job=api('/deforum_api/jobs/'+job_id)
    save('job-latest.json',job)
    if job.get('outdir'):outdir=Path(job['outdir'])
    if job.get('timestring'):prefix=job['timestring']
    advanced=False
    if outdir and prefix:
     while count<N:
      p=outdir/f'{prefix}_{count:09d}.png'
      if not p.exists():break
      try:
       with Image.open(p) as im:a=np.asarray(im.convert('RGB'))
      except (OSError,ValueError):break
      data=memoryview(np.ascontiguousarray(grade(a))).cast('B')
      while data:
       written=process.stdin.write(data)
       if not written:raise RuntimeError('Color encoder pipe closed')
       data=data[written:]
      count+=1;advanced=True
    status('generating-and-coloring',job_id=job_id,raw_frames=count,total_frames=N,job_status=job.get('status'),elapsed=time.time()-began)
    if job.get('status') in ('FAILED','CANCELLED'):raise RuntimeError('Deforum stopped: '+json.dumps(job))
    if job.get('status')=='SUCCEEDED' and count<N:raise RuntimeError(f'Job succeeded with only {count} of {N} readable frames')
    if not advanced:time.sleep(10)
   process.stdin.close()
   if process.wait(timeout=180):raise RuntimeError('Color encoding failed')
 finally:
  if process and process.poll() is None:process.terminate()
 del volume;torch.cuda.empty_cache()
 with av.open(str(raw)) as c:
  s=c.streams.video[0];assert s.average_rate==15;assert sum(1 for _ in c.decode(s))==N
 status('upscaling-and-interpolating',job_id=job_id,raw_frames=N,target_frames=4416)
 cfg=json.loads(Path('D:/AvasthaAI/EnhancerV2/Avastha-Deforum-New-Renderer-4K60.receipt.json').read_text())['config']
 final=R/'Avastha - Deforum ending 650-954 - graded - 720p24.mp4'
 cfg.update(input=str(raw),output=str(final),width=1280,height=720,fps='24',flow_scale=1.0,upscale_device='cuda:0',rife_device='cuda:0',queue_depth=2,pinned_gb=.5)
 save('enhance-config.json',cfg)
 with (R/'enhance.log').open('wb') as log:
  subprocess.run(['D:/AvasthaAI/venv/Scripts/python.exe','-u','D:/AvasthaAI/EnhancerV2/renderer.py','--config',str(R/'enhance-config.json')],env=os.environ.copy(),stdout=log,stderr=subprocess.STDOUT,check=True)
 with av.open(str(final)) as c:
  s=c.streams.video[0];assert s.average_rate==24 and (s.width,s.height)==(1280,720);assert sum(1 for _ in c.decode(s))==4416
 archive=Path(r'\\orc-vst\Avastha-AI\Outputs\DeforumEnding184-20261023');archive.mkdir(parents=True,exist_ok=True)
 status('archiving',job_id=job_id,output=str(final))
 for p in [final,R/'settings.json',R/'submission-receipt.json',R/'color-receipt.json',R/'enhance-config.json',final.with_suffix('.receipt.json'),R/'start.png']:
  dest=archive/p.name;shutil.copy2(p,dest)
  if digest(p)!=digest(dest):raise RuntimeError('Archive hash mismatch: '+p.name)
 status('complete',job_id=job_id,output=str(final),archive=str(archive/final.name),duration=184,frames=4416,fps=24,sha256=digest(final))

if __name__=='__main__':
 try:main()
 except Exception:
  error=traceback.format_exc();print(error,flush=True);status('failed',error=error);raise
