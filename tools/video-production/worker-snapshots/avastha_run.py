from pathlib import Path
import os,json,time,subprocess,hashlib,traceback,math
import av,imageio_ffmpeg
R=Path(__file__).parent
P=Path('D:/AvasthaAI/CosmicRainPilot120-20261026')
SOURCE=Path('D:/AvasthaAI/AcidRework-20261025/Avastha - DRAFT 9m54 - Acid rework and Aleph boom - with music.mp4')
FF=imageio_ffmpeg.get_ffmpeg_exe()
def write(p,v):
    t=p.with_suffix('.tmp');t.write_text(json.dumps(v,indent=2));t.replace(p)
def status(stage,**kw):write(R/'status.json',dict(stage=stage,time=time.time(),**kw))
def main():
    with (R/'worker.lock').open('x') as f:f.write(str(os.getpid()))
    try:
        status('queued-after-cosmic-pilot-finishing')
        while True:
            state=json.loads((P/'finish-status.json').read_text())
            if state['stage']=='failed':raise RuntimeError('Cosmic pilot finisher failed; inspect before acquiring RTX4000')
            if state['stage']=='complete' and not (P/'finish.lock').exists():break
            time.sleep(30)
        with SOURCE.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
        assert digest=='85ac4ac704e167bb52defe3fde0dcadeb038fb90e242a70c249f3515629d9339','Source differs from requested local master'
        write(R/'source-receipt.json',{'source':str(SOURCE),'sha256':digest})
        silent=R/'enhanced-silent.mp4'
        config=json.loads((R/'config.json').read_text())
        config.update(input=str(SOURCE),output=str(silent))
        write(R/'effective-config.json',config)
        if not silent.with_suffix('.receipt.json').exists():
            if silent.exists() or silent.with_name('enhanced-silent.partial.mp4').exists():raise RuntimeError('Unreceipted output exists; inspect before resuming')
            status('enhancing-4k60')
            with (R/'enhance.log').open('ab') as log:
                subprocess.run(['D:/AvasthaAI/venv/Scripts/python.exe','-u','D:/AvasthaAI/EnhancerV2/renderer.py','--config',str(R/'effective-config.json')],stdout=log,stderr=subprocess.STDOUT,check=True)
        status('attaching-original-audio')
        out=R/'Avastha - Acid rework and Aleph boom - 4K60 - with music.mp4'
        subprocess.run([FF,'-v','error','-y','-i',str(silent),'-i',str(SOURCE),'-map','0:v:0','-map','1:a:0','-c','copy','-movflags','+faststart',str(out)],check=True)
        status('verifying')
        with av.open(str(out)) as v:
            s=v.streams.video[0]
            assert (s.width,s.height)==(3840,2160) and s.average_rate==60
            assert len(v.streams.audio)==1
            count=sum(1 for _ in v.decode(s));assert count==35648,count
        subprocess.run([FF,'-v','error','-i',str(out),'-f','null','-'],check=True)
        write(R/'final-receipt.json',{'output':str(out),'frames':count,'fps':60,'size':[3840,2160],'source_sha256':digest,'audio':'Original source AAC stream copied, no audio encoding or retiming','color':'No additional grade','full_decode':True,'video_duration':count/60})
        status('complete',output=str(out))
    finally:(R/'worker.lock').unlink(missing_ok=True)
if __name__=='__main__':
    try:main()
    except Exception:status('failed',error=traceback.format_exc());raise
