from pathlib import Path
import os,subprocess
R=Path('D:/AvasthaAI/ActualDeforum');W=R/'webui';P=W/'venv/Scripts/python.exe'
env=os.environ.copy();env['PATH']=str(R/'git/cmd')+';'+env['PATH'];env['GIT']=str(R/'git/cmd/git.exe');env['CUDA_VISIBLE_DEVICES']='GPU-45dfd7ed-e572-42ff-2a7e-1c36684593f9';env['PIP_CACHE_DIR']=str(R/'pip-cache');env['GRADIO_ANALYTICS_ENABLED']='False'
subprocess.run([str(P),'-m','pip','install','--no-build-isolation','https://github.com/openai/CLIP/archive/d50d76daa670286dd6cacf3bcd80b5e4823fc8e1.zip','numpy==1.26.2'],env=env,check=True)
env['STABLE_DIFFUSION_REPO']='https://github.com/w-e-w/stablediffusion.git'
subprocess.run([str(P),'launch.py','--api','--deforum-api','--nowebui','--port','7865','--opt-sdp-attention','--no-half-vae','--ckpt',str(W/'models/Stable-diffusion/DreamShaper_8_pruned.safetensors')],cwd=W,env=env,check=True)
