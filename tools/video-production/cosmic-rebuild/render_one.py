import json,sys,time,hashlib,shutil,subprocess
from pathlib import Path
import requests
from PIL import Image
import imageio_ffmpeg
ROOT=Path('D:/AvasthaAI/CosmicRebuild-20261030'); C=Path('D:/AvasthaAI/VideoEditFour-20261025'); API='http://127.0.0.1:8192'
shot=int(sys.argv[1]);take=int(sys.argv[2]);assert 1<=shot<=245 and take>=1
plan=json.loads((ROOT/'plan.json').read_text());d=ROOT/f'shot{shot:02d}'/f'take{take:02d}';d.mkdir(parents=True,exist_ok=True)
def save(p,v):p.write_text(json.dumps(v,indent=2),encoding='utf8')
def status(stage,**kw):save(ROOT/'status.json',dict(stage=stage,shot=shot,take=take,time=time.time(),**kw))
if (d/'generated.json').exists():print('Already generated; review existing take');sys.exit()
if shot==1:src=ROOT/'anchor.png'
else:
 a=json.loads((ROOT/f'shot{shot-1:02d}'/'accepted.json').read_text());assert a['visual_review_passed'] and a['real_subject_motion'] and a['continuity_passed'];src=Path(a['continuation_frame'])
override_source=d/'source_override.json'
if override_source.exists():
 v=json.loads(override_source.read_text());assert v.get('reviewed_keyframe') and v.get('reason');src=Path(v['source']);assert src.is_file()
if not (d/'receipt.json').exists():
 assert not (d/'submitting.json').exists(),'Ambiguous submission; inspect Comfy history'
 q=requests.get(API+'/queue',timeout=30).json();assert not q['queue_running'] and not q['queue_pending'],'Comfy busy; do not interrupt'
 im=Image.open(src).convert('RGB');w=round(im.width*736/im.height);im=im.resize((w,736),Image.Resampling.LANCZOS);x=(w-1280)//2
 inp=C/'input'/f'cosmic-rebuild-s{shot}-t{take}.png';im.crop((x,0,x+1280,736)).save(inp)
 n=lambda t,**kw:dict(class_type=t,inputs=kw)
 tag=f'cosmic-rebuild-20261030/s{shot:02d}-t{take:02d}'
 prompt=plan['shots'][shot-1]['prompt'];override=d/'prompt.txt'
 if override.exists():prompt=override.read_text(encoding='utf8')
 extra_negative=(d/'negative.txt').read_text(encoding='utf8') if (d/'negative.txt').exists() else ''
 settings=json.loads((d/'settings.json').read_text()) if (d/'settings.json').exists() else {}
 cfg=float(settings.get('cfg',5));steps=int(settings.get('steps',20))
 assert 4<=cfg<=7 and 20<=steps<=32,'Outside bounded reviewed settings'
 g={'1':n('UNETLoader',unet_name='wan2.2_ti2v_5B_fp16.safetensors',weight_dtype='default'),'2':n('CLIPLoader',clip_name='umt5_xxl_fp8_e4m3fn_scaled.safetensors',type='wan',device='default'),'3':n('VAELoader',vae_name='wan2.2_vae.safetensors'),'4':n('CLIPTextEncode',clip=['2',0],text=prompt+' '+plan['style']),'5':n('CLIPTextEncode',clip=['2',0],text='frozen image, static subjects, slideshow, zoom, camera movement, cuts, collage, flicker, strobe, extra hands, distorted fingers, melting faces, text, watermark, '+extra_negative),'6':n('LoadImage',image=inp.name),'7':n('Wan22ImageToVideoLatent',vae=['3',0],width=1280,height=736,length=plan['shots'][shot-1]['length'],batch_size=1,start_image=['6',0]),'8':n('ModelSamplingSD3',model=['1',0],shift=8),'9':n('KSampler',model=['8',0],positive=['4',0],negative=['5',0],latent_image=['7',0],seed=143603000+shot*100+take,steps=steps,cfg=cfg,sampler_name='uni_pc',scheduler='simple',denoise=1),'90':n('SaveLatent',samples=['9',0],filename_prefix=tag),'10':n('VAEDecodeTiled',samples=['90',0],vae=['3',0],tile_size=512,overlap=64,temporal_size=4096,temporal_overlap=4),'11':n('SaveImage',images=['10',0],filename_prefix=tag+'/f')}
 save(d/'workflow.json',g);save(d/'submitting.json',{'time':time.time()});r=requests.post(API+'/prompt',json={'prompt':g,'client_id':'cosmic-rebuild-review-gated'},timeout=90);r.raise_for_status();save(d/'receipt.json',r.json())
pid=json.loads((d/'receipt.json').read_text())['prompt_id'];status('rendering',prompt_id=pid)
while True:
 h=requests.get(API+'/history/'+pid,timeout=60).json().get(pid)
 if h:
  save(d/'history.json',h);assert h['status']['status_str']=='success',h['status'];break
 time.sleep(15)
h=json.loads((d/'history.json').read_text());imgs=h['outputs']['11']['images'];frames=[str(C/'output'/im['subfolder']/im['filename']) for im in imgs];assert len(frames)==plan['shots'][shot-1]['length']
save(d/'generated.json',{'frames':frames,'deliver_frames':plan['shots'][shot-1]['deliver_frames'],'source':str(src),'prompt_id':pid,'review_passed':False})
ff=imageio_ffmpeg.get_ffmpeg_exe();out=d/'review-with-music.mp4';start=plan['shots'][shot-1]['frames'][0]/24; need=plan['shots'][shot-1]['deliver_frames']
cmd=[ff,'-v','error','-y','-framerate','24','-i',str(Path(frames[0]).parent/'f_%05d_.png'),'-ss',str(start),'-i',plan['track'],'-map','0:v','-map','1:a','-vf','crop=1280:720:0:8','-frames:v',str(need),'-t',str(need/24),'-c:v','libx264','-crf','16','-pix_fmt','yuv420p','-c:a','aac','-b:a','320k','-movflags','+faststart',str(out)]
subprocess.run(cmd,check=True);subprocess.run([ff,'-v','error','-xerror','-i',str(out),'-f','null','-'],check=True)
status('awaiting-visual-review',output=str(out),note='No next shot launches automatically. Review raw motion and neighbour continuity, write accepted.json only if passed.')
print(out,flush=True)
