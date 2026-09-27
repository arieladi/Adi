// Loads avc.meters.js with Max globals stubbed and checks the designed filters
// against the IEC 61672 / BS.1770 reference curves.
const fs=require('fs'),vm=require('vm');
const src=fs.readFileSync('device/avc.meters.js','utf8');
const ctx={Math,isFinite,post:()=>{},outlet:()=>{},messnamed:()=>{},
           jsarguments:['avc.meters.js'],console};
ctx.global=ctx; vm.createContext(ctx); vm.runInContext(src,ctx);

const A_REF={31.5:-39.4,63:-26.2,100:-19.1,200:-10.9,500:-3.2,1000:0.0,
             2000:1.2,4000:1.0,8000:-1.1,10000:-2.5};
const C_REF={31.5:-3.0,63:-0.8,125:-0.2,250:0.0,1000:0.0,4000:-0.8,8000:-3.0,10000:-4.4};

function db(secs,f,sr){return 20*Math.log10(ctx.cascadeMag(secs,f,sr));}
let fails=0;
for(const sr of [44100,48000,96000]){
  const A=ctx.designA(sr), C=ctx.designC(sr);
  console.log(`\n--- sr=${sr} ---`);
  for(const [f,ref] of Object.entries(A_REF)){
    const got=db(A,+f,sr), err=got-ref;
    const tol=(+f>=8000&&sr<96000)?1.5:0.35;
    const ok=Math.abs(err)<=tol; if(!ok)fails++;
    console.log(`  A ${String(f).padStart(6)} Hz  ref ${ref.toFixed(1).padStart(6)}  got ${got.toFixed(2).padStart(7)}  err ${err.toFixed(2).padStart(6)} ${ok?'':'  <-- FAIL'}`);
  }
  for(const [f,ref] of Object.entries(C_REF)){
    const got=db(C,+f,sr), err=got-ref;
    const tol=(+f>=8000&&sr<96000)?1.5:0.35;
    const ok=Math.abs(err)<=tol; if(!ok)fails++;
    console.log(`  C ${String(f).padStart(6)} Hz  ref ${ref.toFixed(1).padStart(6)}  got ${got.toFixed(2).padStart(7)}  err ${err.toFixed(2).padStart(6)} ${ok?'':'  <-- FAIL'}`);
  }
  const K=ctx.designK(sr,false);
  console.log(`  K   20 Hz ${db(K,20,sr).toFixed(2)}   100 Hz ${db(K,100,sr).toFixed(2)}   1k ${db(K,1000,sr).toFixed(2)}   4k ${db(K,4000,sr).toFixed(2)}  10k ${db(K,10000,sr).toFixed(2)}`);
  const Kn=ctx.designK(sr,true);
  console.log(`  K(normalised) 1k = ${db(Kn,1000,sr).toFixed(4)} dB  (must be 0)`);
}
console.log(fails? `\n${fails} FAILURES`:"\nALL WEIGHTING CHECKS PASSED");
process.exit(fails?1:0);
