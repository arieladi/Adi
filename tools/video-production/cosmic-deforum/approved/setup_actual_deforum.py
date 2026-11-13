from pathlib import Path
import urllib.request,json,zipfile,subprocess,os
R=Path('D:/AvasthaAI/ActualDeforum');R.mkdir(exist_ok=True)
def get(url):
 with urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'Avastha-local-setup'}),timeout=90) as f:return json.load(f)
def download(url,p):
 if not p.exists():
  print('DOWNLOAD',p.name,flush=True);urllib.request.urlretrieve(url,p)
gitdir=R/'git'
if not (gitdir/'cmd/git.exe').exists():
 rel=get('https://api.github.com/repos/git-for-windows/git/releases/latest');asset=next(a for a in rel['assets'] if a['name'].startswith('MinGit-') and a['name'].endswith('64-bit.zip') and 'busybox' not in a['name'])
 p=R/'mingit.zip';download(asset['browser_download_url'],p)
 with zipfile.ZipFile(p) as z:z.extractall(gitdir)
env=os.environ.copy();env['PATH']=str(gitdir/'cmd')+';'+env['PATH'];env['UV_PYTHON_INSTALL_DIR']=str(R/'python');env['UV_CACHE_DIR']=str(R/'uv-cache');env['PIP_CACHE_DIR']=str(R/'pip-cache')
uv=next(Path('D:/AvasthaAI').rglob('uv.exe'));subprocess.run([str(uv),'python','install','3.10'],env=env,check=True)
web=R/'webui';git=str(gitdir/'cmd/git.exe')
for url,p in [('https://github.com/AUTOMATIC1111/stable-diffusion-webui',web),('https://github.com/deforum/sd-webui-deforum',web/'extensions/deforum')]:
 if not p.exists():subprocess.run([git,'clone','--depth','1',url,str(p)],env=env,check=True)
if not (web/'venv/Scripts/python.exe').exists():subprocess.run([str(uv),'venv','--python','3.10','--seed',str(web/'venv')],env=env,check=True)
files=get('https://huggingface.co/api/models/Lykon/DreamShaper')['siblings'];names=[x['rfilename'] for x in files if x['rfilename'].endswith('.safetensors')];print('CHECKPOINTS',names,flush=True)
(R/'environment.json').write_text(json.dumps({'git':git,'uv':str(uv),'webui':str(web)}))
print('BASE_READY',flush=True)
