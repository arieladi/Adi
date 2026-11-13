from pathlib import Path
import json, urllib.request, time

ROOT=Path('D:/AvasthaAI/DeforumSection050-123')
receipt=ROOT/'job.json'
marker=ROOT/'submission-started.json'
if receipt.exists() or marker.exists():
    raise SystemExit('Existing submission marker/receipt: inspect before any repeat submission')
s=json.loads(Path('D:/AvasthaAI/ActualDeforum/settings-8s.json').read_text())
s.update(batch_name='Avastha_Deforum_050_123',max_frames=495,
         video_init_path=str(ROOT/'source-480p15.mp4'),
         hybrid_generate_inputframes=True,hybrid_use_first_frame_as_init_image=True,
         hybrid_composite='Normal',hybrid_motion='None',hybrid_use_init_image=False,
         overwrite_extracted_frames=True,
         hybrid_comp_alpha_schedule='0:(1.0), 15:(0.70), 30:(0.25), 45:(0.05), 450:(0.05), 465:(0.25), 480:(0.70), 494:(1.0)',
         strength_schedule='0:(0.98), 15:(0.88), 30:(0.74), 45:(0.62), 80:(0.55), 105:(0.48), 135:(0.55), 164:(0.60), 200:(0.55), 225:(0.48), 255:(0.55), 284:(0.60), 320:(0.55), 345:(0.48), 375:(0.55), 404:(0.60), 430:(0.55), 450:(0.60), 465:(0.74), 480:(0.88), 494:(0.98)',
         noise_schedule='0:(0), 45:(0.02), 450:(0.02), 494:(0)',
         zoom='0:(1.0), 45:(1.005), 450:(1.005), 494:(1.0)',
         angle='0:(0), 45:(0.06), 450:(0.06), 494:(0)',
         translation_x='0:(0), 45:(0.12), 450:(0.12), 494:(0)',
         translation_y='0:(0), 45:(-0.05), 450:(-0.05), 494:(0)')
p=list(s['prompts'].values())
s['prompts']={str(k):p[i] for k,i in [(0,0),(95,1),(155,2),(215,0),(275,1),(335,2),(395,0),(450,0)]}
(ROOT/'settings.json').write_text(json.dumps(s,indent=2))
marker.write_text(json.dumps({'started':time.time(),'batch_name':s['batch_name']}))
request=urllib.request.Request('http://127.0.0.1:7865/deforum_api/batches',
    data=json.dumps({'deforum_settings':s,'options_overrides':{'img2img_fix_steps':True}}).encode(),
    headers={'Content-Type':'application/json'})
with urllib.request.urlopen(request,timeout=120) as response:
    result=json.load(response)
receipt.write_text(json.dumps(result,indent=2))
print(json.dumps(result),flush=True)
