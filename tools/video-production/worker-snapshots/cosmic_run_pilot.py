from pathlib import Path
import json,os,time,requests,shutil,subprocess,traceback
R=Path(__file__).parent; C=Path('D:/AvasthaAI/VideoEditFour-20261025');API='http://127.0.0.1:8192'
def save(p,v):
 q=p.with_suffix('.tmp');q.write_text(json.dumps(v,indent=2),encoding='utf-8');q.replace(p)
def status(stage,**kw):save(R/'generation-status.json',dict(stage=stage,time=time.time(),**kw))
def node(t,**kw):return dict(class_type=t,inputs=kw)
def main():
 with (R/'worker.lock').open('x') as f:f.write(str(os.getpid()))
 try:
  plan=json.loads((R/'plan.json').read_text());q=requests.get(API+'/queue',timeout=30).json()
  assert not q['queue_running'] and not q['queue_pending'],'Existing Comfy jobs must finish first'
  for s in plan['shots']:
   i=s['index'];d=R/'shots'/f'{i:02d}';d.mkdir(parents=True,exist_ok=True)
   raw=C/'generated'/'cosmic-pilot-120'/f'{i:02d}'
   if (d/'generated.json').exists():continue
   if shutil.disk_usage(R).free<8*1024**3:raise RuntimeError('Less than8GiB free on D; preserve outputs and inspect')
   if s['reference'] is None:
    prior=sorted((C/'generated'/'cosmic-pilot-120'/f'{i-1:02d}').glob('*.png'));assert len(prior)==121
    source=prior[-1]
   else:source=R/'references'/f"boundary-{s['reference']:03d}.png"
   inp=C/'input'/f'cosmic-pilot-120-{i:02d}.png';shutil.copy2(source,inp)
   g={
    '1':node('UNETLoader',unet_name='wan2.2_ti2v_5B_fp16.safetensors',weight_dtype='default'),
    '2':node('CLIPLoader',clip_name='umt5_xxl_fp8_e4m3fn_scaled.safetensors',type='wan',device='default'),
    '3':node('VAELoader',vae_name='wan2.2_vae.safetensors'),
    '4':node('CLIPTextEncode',clip=['2',0],text=s['prompt']),
    '5':node('CLIPTextEncode',clip=['2',0],text='text, watermark, subtitles, collage, split screen, inset pictures, random duplicates, strobe, flicker, static image, blurry, muddy detail, deformed geometry, abrupt cuts'),
    '6':node('LoadImage',image=inp.name),
    '7':node('Wan22ImageToVideoLatent',vae=['3',0],width=1280,height=736,length=121,batch_size=1,start_image=['6',0]),
    '8':node('ModelSamplingSD3',model=['1',0],shift=8),
    '9':node('KSampler',model=['8',0],positive=['4',0],negative=['5',0],latent_image=['7',0],seed=143261200+i,steps=20,cfg=5,sampler_name='uni_pc',scheduler='simple',denoise=1),
    '90':node('SaveLatent',samples=['9',0],filename_prefix=f'cosmic-pilot-120/shot-{i:02d}'),
    '10':node('VAEDecodeTiled',samples=['90',0],vae=['3',0],tile_size=512,overlap=64,temporal_size=4096,temporal_overlap=4),
    '11':node('AvasthaSave',images=['10',0],tag=f'cosmic-pilot-120/{i:02d}')}
   receipt=d/'receipt.json'
   if receipt.exists():pid=json.loads(receipt.read_text())['prompt_id']
   else:
    assert not (d/'submitting.json').exists(),'Ambiguous existing submission; inspect queue/history'
    save(d/'workflow.json',g);save(d/'submitting.json',{'time':time.time()})
    response=requests.post(API+'/prompt',json={'prompt':g,'client_id':'cosmic-pilot-120'},timeout=90)
    save(receipt,response.json());response.raise_for_status();pid=response.json()['prompt_id']
   status('rendering',shot=i+1,total=len(plan['shots']),prompt_id=pid,absolute_interval=[s['start'],s['end']])
   while True:
    h=requests.get(API+'/history/'+pid,timeout=90).json()
    if pid in h:
     save(d/'history.json',h[pid]);assert h[pid]['status']['status_str']=='success',h[pid]['status'];break
    time.sleep(20)
   paths=sorted(raw.glob('*.png'));assert len(paths)==121
   save(d/'generated.json',{'raw':str(raw),'frames':121,'native':[1280,736],'source':str(source),'latent_saved':True})
  status('complete',shots=len(plan['shots']))
 finally:(R/'worker.lock').unlink(missing_ok=True)
if __name__=='__main__':
 try:main()
 except Exception:status('failed',error=traceback.format_exc());raise
