import json,time,subprocess,os
from pathlib import Path
import requests
r=Path('D:/AvasthaAI/CosmicRebuild-20261030');lock=r/'first-launch.lock'
with lock.open('x') as f:f.write(str(os.getpid()))
try:
 while True:
  q=requests.get('http://127.0.0.1:8192/queue',timeout=60).json()
  if not q['queue_running'] and not q['queue_pending']:break
  (r/'launch-status.json').write_text(json.dumps({'stage':'waiting-existing-job','time':time.time()}));time.sleep(30)
 (r/'launch-status.json').write_text(json.dumps({'stage':'first-shot-launched','time':time.time()}))
 with (r/'first-shot.log').open('a') as log:
  result=subprocess.run(['D:/AvasthaAI/venv/Scripts/python.exe','-u',str(r/'render_one.py'),'1','1'],stdout=log,stderr=subprocess.STDOUT)
 (r/'launch-status.json').write_text(json.dumps({'stage':'awaiting-review' if result.returncode==0 else 'failed','code':result.returncode,'time':time.time()}))
finally:lock.unlink(missing_ok=True)
