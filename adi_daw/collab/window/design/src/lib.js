// Shared drawing parts for ADI's design sheets (17 November 2026). ADI's own drawing.
const C={win:'#1b1b1b',bar:'#242424',panel:'#2b2b2b',lane:'#323232',lane2:'#2e2e2e',ctl:'#3d3d3d',edge:'#4a4a4a',line:'#111111',well:'#161616',grp:'#2f2f2f',grpEdge:'#454545',
  head:'#383838',text:'#dcdcdc',dim:'#8f8f8f',faint:'#5c5c5c',ink:'#1a1a1a',acc:'#ffad3b',val:'#4fc3ea',rec:'#ff4a4a',grn:'#62d36b',yel:'#f2c53d',mod:'#c479ff'};
function rng(seed){let a=seed>>>0;return()=>{a|=0;a=a+0x6D2B79F5|0;let t=Math.imul(a^a>>>15,1|a);t=t+Math.imul(t^t>>>7,61|t)^t;return((t^t>>>14)>>>0)/4294967296}}
let UID=0;
const R=(x,y,w,h,f,o={})=>`<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="${o.r||0}" fill="${f}"${o.s?` stroke="${o.s}" stroke-width="${o.sw||1}"`:''}${o.op!=null?` opacity="${o.op}"`:''}${o.d?` stroke-dasharray="${o.d}"`:''}/>`;
const T=(x,y,t,o={})=>`<text x="${x}" y="${y}" font-size="${o.z||12}" fill="${o.f||C.text}" text-anchor="${o.a||'start'}" font-weight="${o.w||400}"${o.ls?` letter-spacing="${o.ls}"`:''}${o.op!=null?` opacity="${o.op}"`:''}>${t}</text>`;
const L=(x1,y1,x2,y2,c,w=1,o={})=>`<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke="${c}" stroke-width="${w}"${o.op!=null?` opacity="${o.op}"`:''}${o.cap?` stroke-linecap="${o.cap}"`:''}/>`;
const P=(d,o={})=>`<path d="${d}" fill="${o.f||'none'}"${o.s?` stroke="${o.s}" stroke-width="${o.sw||1}"`:''}${o.op!=null?` opacity="${o.op}"`:''} stroke-linejoin="round" stroke-linecap="round"/>`;
const CI=(x,y,r,f,o={})=>`<circle cx="${x}" cy="${y}" r="${r}" fill="${f}"${o.s?` stroke="${o.s}" stroke-width="${o.sw||1}"`:''}${o.op!=null?` opacity="${o.op}"`:''}/>`;
const pol=(cx,cy,r,deg)=>{const a=(deg-90)*Math.PI/180;return[cx+r*Math.cos(a),cy+r*Math.sin(a)]};
function arc(cx,cy,r,a0,a1){const[p0x,p0y]=pol(cx,cy,r,a0),[p1x,p1y]=pol(cx,cy,r,a1);return`M${p0x.toFixed(2)} ${p0y.toFixed(2)}A${r} ${r} 0 ${Math.abs(a1-a0)>180?1:0} ${a1>a0?1:0} ${p1x.toFixed(2)} ${p1y.toFixed(2)}`}
const DEFS=()=>`<defs>
<radialGradient id="dial" cx="35%" cy="28%" r="80%"><stop offset="0" stop-color="#565660"/><stop offset="1" stop-color="#28282c"/></radialGradient>
<radialGradient id="dialh" cx="35%" cy="28%" r="80%"><stop offset="0" stop-color="#6c6c76"/><stop offset="1" stop-color="#35353a"/></radialGradient>
<linearGradient id="mv" x1="0" y1="1" x2="0" y2="0"><stop offset="0" stop-color="#3fae5a"/><stop offset=".72" stop-color="#b9cf3f"/><stop offset=".9" stop-color="#f2a93b"/><stop offset="1" stop-color="#ff5a4a"/></linearGradient>
<linearGradient id="mh" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#3fae5a"/><stop offset=".72" stop-color="#b9cf3f"/><stop offset="1" stop-color="#f2a93b"/></linearGradient>
<filter id="sh" x="-6%" y="-6%" width="112%" height="124%"><feDropShadow dx="0" dy="4" stdDeviation="7" flood-color="#000" flood-opacity=".6"/></filter>
</defs>`;
// The ring knob: the value is the ring.
function knob(cx,cy,r,v,col,label,val,o={}){let g='';const a0=-135,a1=135,av=a0+(a1-a0)*v,rw=o.ring||Math.max(2.2,r*0.17);
  g+=P(arc(cx,cy,r,a0,a1),{s:'#101012',sw:rw});
  if(o.bi)g+=P(arc(cx,cy,r,Math.min(0,av),Math.max(0,av)+0.01),{s:col,sw:rw});else g+=P(arc(cx,cy,r,a0,av+0.01),{s:col,sw:rw,op:o.dis?0.35:1});
  if(o.mod){const m0=a0+(a1-a0)*o.mod[0],m1=a0+(a1-a0)*o.mod[1];g+=P(arc(cx,cy,r+rw+1.5,m0,m1),{s:C.mod,sw:1.6})}
  g+=CI(cx,cy,r-rw-1.5,o.hover?'url(#dialh)':'url(#dial)',{s:'#101012'});
  const[px,py]=pol(cx,cy,r-rw-3,av),[qx,qy]=pol(cx,cy,(r-rw-3)*0.3,av);g+=L(qx,qy,px,py,o.dis?'#777':'#f0f0f0',Math.max(1.4,r*0.09),{cap:'round'});
  if(o.auto)g+=CI(cx+r,cy-r,2.6,C.rec);
  if(label)g+=T(cx,cy+r+13,label,{z:o.lz||10.5,f:C.dim,a:'middle'});
  if(val)g+=T(cx,cy+r+(label?13:2)+(o.lz||10.5)+2.5,val,{z:o.lz||10.5,f:o.dis?C.faint:C.text,a:'middle'});return g}
const btn=(x,y,w,h,t,o={})=>R(x,y,w,h,o.on?o.c||C.acc:o.fill||C.ctl,{r:o.r==null?5:o.r,s:o.on?null:o.s||C.edge})+(t?T(x+w/2,y+h/2+(o.z||11.5)*0.36,t,{z:o.z||11.5,a:'middle',f:o.on?C.ink:o.f||C.text,w:o.w||600}):'');
const pill=(x,y,w,h,t,o={})=>R(x,y,w,h,o.on?o.c||C.acc:C.head,{r:h/2,s:o.on?null:C.edge})+T(x+w/2,y+h/2+3.8,t,{z:o.z||10.5,a:'middle',f:o.on?C.ink:o.f||C.text,w:o.on?600:400});
const box=(x,y,w,h,t,o={})=>R(x,y,w,h,C.well,{r:4,s:'#3a3a3a'})+(o.fill!=null?R(x+1,y+1,(w-2)*o.fill,h-2,C.val,{r:3,op:0.28}):'')+T(x+w/2,y+h/2+(o.z||10.5)*0.36,t,{z:o.z||10.5,a:'middle',f:o.f||C.text});
const chooser=(x,y,w,h,t,o={})=>R(x,y,w,h,C.ctl,{r:4,s:C.edge})+T(x+6,y+h/2+(o.z||10.5)*0.36,t,{z:o.z||10.5,f:o.f||C.text})+P(`M${x+w-12} ${y+h/2-2}l3.5 3.5l3.5 -3.5`,{s:C.dim,sw:1.5});
const grp=(x,y,w,h)=>R(x,y,w,h,C.grp,{r:8,s:C.grpEdge});
const sep=(x,y,h)=>L(x,y+4,x,y+h-4,'#484848');
const meterV=(x,y,w,h,v,pk)=>R(x,y,w,h,'#0d0d0f',{r:1.5})+`<rect x="${x}" y="${(y+h-h*v).toFixed(1)}" width="${w}" height="${(h*v).toFixed(1)}" rx="1.5" fill="url(#mv)"/>`+(pk?R(x,y+h-h*pk,w,1.5,'#f3f3f3'):'');
const meterH=(x,y,w,h,v,pk)=>R(x,y,w,h,'#0d0d0f',{r:1.5})+`<rect x="${x}" y="${y}" width="${(w*v).toFixed(1)}" height="${h}" rx="1.5" fill="url(#mh)"/>`+(pk?R(x+w*pk,y,1.5,h,'#f3f3f3'):'');
const note=(x,y,n)=>CI(x,y,11,'#ffffff')+`<text x="${x}" y="${y+4.5}" font-size="13" font-weight="700" text-anchor="middle" fill="#101013">${n}</text>`;
// icons, drawn round and a little heavy
const iPlay=(x,y,c)=>P(`M${x-5} ${y-7}L${x+7} ${y}L${x-5} ${y+7}Z`,{f:c,s:c,sw:2.6});
const iStop=(x,y,c)=>R(x-6.5,y-6.5,13,13,c,{r:2.8});
const iRec=(x,y,c)=>CI(x,y,7,c);
const iPlus=(x,y,c)=>P(`M${x-7} ${y}h14M${x} ${y-7}v14`,{s:c,sw:2.2});
const iArm=(x,y,c)=>P(`M${x-7} ${y+5}c5 0 3 -10 8 -10`,{s:c,sw:2.2})+CI(x+5,y-5,2.6,c);
const iBack=(x,y,c)=>P(`M${x+7} ${y}h-13M${x-6} ${y}l5 -5M${x-6} ${y}l5 5`,{s:c,sw:2.2});
const iLoop=(x,y,c)=>P(`M${x-8} ${y+5}v-6a4 4 0 0 1 4 -4h8a4 4 0 0 1 4 4v6`,{s:c,sw:2.2});
const iIn=(x,y,c)=>P(`M${x-7} ${y+5}h5c3 0 3 -10 6 -10h4`,{s:c,sw:2.2});
const iOut=(x,y,c)=>P(`M${x-7} ${y-5}h4c3 0 3 10 6 10h5`,{s:c,sw:2.2});
const iUndo=(x,y,c)=>P(`M${x-6} ${y-2}h8a4.5 4.5 0 0 1 0 9h-5M${x-6} ${y-2}l4 -4M${x-6} ${y-2}l4 4`,{s:c,sw:2.1});
const iRedo=(x,y,c)=>P(`M${x+6} ${y-2}h-8a4.5 4.5 0 0 0 0 9h5M${x+6} ${y-2}l-4 -4M${x+6} ${y-2}l-4 4`,{s:c,sw:2.1});
const iPen=(x,y,c)=>P(`M${x-6} ${y+6}l2 -5l8 -8l3 3l-8 8z`,{s:c,sw:1.9});
const iKeys=(x,y,c)=>R(x-8,y-6,16,12,'none',{r:2,s:c,sw:1.8})+L(x-3,y-6,x-3,y+1,c,1.8)+L(x+3,y-6,x+3,y+1,c,1.8);
const iMetro=(x,y,c)=>CI(x-4,y,4,'none',{s:c,sw:1.9})+CI(x+6,y,3.2,c);
const iSwap=(x,y,c)=>P(`M${x-7} ${y-3}h14l-3 -3M${x+7} ${y+3}h-14l3 3`,{s:c,sw:1.6});
const iPower=(x,y,c)=>CI(x,y,5,'none',{s:c,sw:1.8})+L(x,y-6,x,y-1,c,1.8,{cap:'round'});
// The menu bar and the Control Bar, the same in every view. `view` is 'arr' or 'mix'.
function topBars(W,title,view){let s=R(0,0,W,24,'#0f0f10');s+=T(12,16,'ADI',{w:700,z:12});
  ['File','Edit','Create','View','Options','Navigate','Help'].forEach((m,i)=>s+=T(58+i*62,16,m,{z:12,f:C.dim}));s+=T(W/2,16,title,{z:12,f:C.dim,a:'middle'});
  const by=24,bh=44;s+=R(0,by,W,bh,C.bar);s+=L(0,by+bh,W,by+bh,C.line);let x=12;const cy=by+bh/2,t=by+8,h=28;
  s+=grp(x,t,318,h);s+=T(x+26,cy+4,'Link',{z:11.5,a:'middle',f:C.dim,w:600});s+=sep(x+52,t,h);s+=T(x+74,cy+4,'Tap',{z:11.5,a:'middle',f:C.dim,w:600});s+=sep(x+96,t,h);
  s+=T(x+130,cy+4,'120.00',{z:12,a:'middle',f:C.val,w:600});s+=sep(x+164,t,h);s+=T(x+192,cy+4,'4 / 4',{z:12,a:'middle',w:600});s+=sep(x+220,t,h);s+=iMetro(x+244,cy,C.dim);s+=sep(x+266,t,h);s+=T(x+292,cy+4,'1 Bar',{z:11.5,a:'middle',f:C.dim,w:600});
  x=340;s+=grp(x,t,150,h);s+=T(x+12,cy+4,'Key',{z:11,f:C.dim});s+=T(x+42,cy+4,'C#',{z:12,w:600});s+=sep(x+70,t,h);s+=T(x+110,cy+4,'Minor',{z:11.5,a:'middle',f:C.dim,w:600});
  x=640;s+=grp(x,t,36,h);s+=iBack(x+18,cy,C.dim);s+=btn(x+42,t,84,h,'1.  1.  1',{fill:C.well,s:'#3a3a3a',f:C.text,r:6});
  x=776;s+=grp(x,t,150,h);s+=iPlay(x+25,cy,C.grn);s+=sep(x+50,t,h);s+=iStop(x+75,cy,C.text);s+=sep(x+100,t,h);s+=iRec(x+125,cy,C.rec);
  x=936;s+=grp(x,t,200,h);s+=iPlus(x+25,cy,C.text);s+=sep(x+50,t,h);s+=R(x+52,t+2,46,h-4,C.acc,{r:6});s+=iArm(x+75,cy,C.ink);s+=sep(x+100,t,h);s+=iBack(x+125,cy,C.dim);s+=sep(x+150,t,h);s+=CI(x+175,cy,6.5,'none',{s:C.text,sw:2});
  x=1150;s+=btn(x,t,84,h,'3.  1.  1',{fill:C.well,s:'#3a3a3a',r:6});s+=grp(x+90,t,116,h);s+=iIn(x+110,cy,C.dim);s+=sep(x+129,t,h);s+=R(x+131,t+2,36,h-4,C.acc,{r:6});s+=iLoop(x+149,cy,C.ink);s+=sep(x+168,t,h);s+=iOut(x+188,cy,C.dim);s+=btn(x+212,t,84,h,'4.  0.  0',{fill:C.well,s:'#3a3a3a',r:6});
  x=1462;s+=grp(x,t,76,h);s+=iUndo(x+20,cy,C.text);s+=sep(x+38,t,h);s+=iRedo(x+57,cy,C.dim);
  // the view switch
  x=1548;s+=grp(x,t,186,h);const on=view==='mix'?1:0;s+=R(x+2+on*92,t+2,90,h-4,C.acc,{r:6});s+=T(x+47,cy+4,'Arrangement',{z:11.5,a:'middle',w:700,f:on?C.dim:C.ink});s+=T(x+139,cy+4,'Mix View',{z:11.5,a:'middle',w:700,f:on?C.ink:C.dim});
  x=1744;s+=grp(x,t,76,h);s+=iPen(x+20,cy,C.dim);s+=sep(x+38,t,h);s+=iKeys(x+58,cy,C.dim);
  s+=grp(1830,t,78,h);s+=T(1850,cy+4,'MIDI',{z:11,a:'middle',f:C.dim,w:600});s+=sep(1870,t,h);s+=T(1889,cy+4,'6 %',{z:11,a:'middle',f:C.dim,w:600});return s}
// Displays.
function eqd(x,y,w,h,bands,seed,o={}){let g='';const r=rng(seed);const mid=y+h*0.5,id='c'+(UID++);
  g+=`<clipPath id="${id}"><rect x="${x}" y="${y}" width="${w}" height="${h}" rx="${o.r||5}"/></clipPath><g clip-path="url(#${id})">`;g+=R(x,y,w,h,C.well);
  if(!o.mini){for(let k=1;k<10;k++){const gx=x+w*Math.log10(1+k*1.9)/Math.log10(20);g+=L(gx,y,gx,y+h,'#ffffff',1,{op:0.04})}for(let k=1;k<4;k++)g+=L(x,y+h*k/4,x+w,y+h*k/4,'#ffffff',1,{op:0.05})}
  let sp=`M${x} ${y+h}`,n=o.mini?40:100,lv=0.5;const tilt=o.tilt==null?0.5:o.tilt;for(let k=0;k<=n;k++){const u=k/n;lv=lv*0.72+0.28*r();const env=Math.sin(Math.PI*Math.pow(u,0.8))*(1-tilt*u*0.6);sp+=`L${(x+w*u).toFixed(1)} ${(y+h-h*0.62*env*(0.45+0.75*lv)).toFixed(1)}`}sp+=`L${x+w} ${y+h}Z`;g+=P(sp,{f:'#ffffff',op:0.10});g+=P(sp,{s:'#cfcfcf',sw:0.8,op:0.35});
  const one=(b,u)=>b.t==='hp'?-1.4*Math.max(0,(b.f-u)/0.12):b.t==='hs'?b.g/(1+Math.exp((b.f-u)/0.04)):b.t==='ls'?b.g/(1+Math.exp((u-b.f)/0.04)):b.g*Math.exp(-Math.pow((u-b.f)/(b.q||0.07),2));
  const resp=u=>bands.reduce((v,b)=>v+one(b,u),0);let d='';const m=140;
  if(!o.mini)for(const b of bands){if(b.t==='hp')continue;let bd=`M${x} ${mid}`;for(let k=0;k<=m;k++){const u=k/m;bd+=`L${(x+w*u).toFixed(1)} ${(mid-one(b,u)*h*0.36).toFixed(1)}`}g+=P(bd+`L${x+w} ${mid}Z`,{f:b.c,op:0.22})}
  for(let k=0;k<=m;k++){const u=k/m,yy=Math.max(y-4,Math.min(y+h+4,mid-resp(u)*h*0.36));d+=(k?'L':'M')+(x+w*u).toFixed(1)+' '+yy.toFixed(1)}
  if(o.mini)g+=P(d+`L${x+w} ${mid}L${x} ${mid}Z`,{f:C.yel,op:0.10});else g+=L(x,mid,x+w,mid,'#ffffff',1,{op:0.12});
  if(bands.length)g+=P(d,{s:C.yel,sw:o.mini?1.3:2});
  if(!o.mini)for(const b of bands){const yy=mid-resp(b.f)*h*0.36;g+=CI((x+w*b.f).toFixed(1),Math.max(y+7,Math.min(y+h-7,yy)).toFixed(1),o.dot||5.5,b.c,{s:C.well,sw:1.5})}
  return g+'</g>'}
function dynd(x,y,w,h,seed,o={}){let g='';const r=rng(seed),id='d'+(UID++);
  g+=`<clipPath id="${id}"><rect x="${x}" y="${y}" width="${w}" height="${h}" rx="5"/></clipPath><g clip-path="url(#${id})">`;g+=R(x,y,w,h,C.well);
  let lv=`M${x} ${y+h}`,gr=`M${x} ${y}`,n=Math.max(20,Math.floor(w/2.6)),a=0.5,q=0;const dep=o.depth==null?0.4:o.depth;
  for(let k=0;k<=n;k++){const u=k/n;a=a*0.6+0.4*r();const hit=(k%(o.per||10))<2?1:0;q=Math.max(q*0.8,hit*(0.5+0.5*r()));const e=0.35+0.4*a+0.22*q;
    lv+=`L${(x+w*u).toFixed(1)} ${(y+h-h*0.8*e).toFixed(1)}`;gr+=`L${(x+w*u).toFixed(1)} ${(y+h*dep*q*(0.6+0.4*a)+1).toFixed(1)}`}
  g+=P(lv+`L${x+w} ${y+h}Z`,{f:o.col||'#9db3c9',op:0.22});g+=P(lv,{s:o.col||'#b9c7d6',sw:0.8,op:0.5});g+=P(gr+`L${x+w} ${y}Z`,{f:C.rec,op:o.mini?0.2:0.28});g+=P(gr,{s:C.rec,sw:o.mini?0.8:1.1,op:o.mini?0.8:1});
  if(!o.mini&&!o.noth)g+=L(x,y+h*0.34,x+w,y+h*0.34,C.acc,1,{op:0.7});return g+'</g>'}
function curved(x,y,w,h){let g=R(x,y,w,h,C.well,{r:5});for(let k=1;k<4;k++){g+=L(x+w*k/4,y,x+w*k/4,y+h,'#ffffff',1,{op:0.05});g+=L(x,y+h*k/4,x+w,y+h*k/4,'#ffffff',1,{op:0.05})}
  g+=L(x,y+h,x+w,y,'#ffffff',1,{op:0.12});const tx=x+w*0.55,ty=y+h*0.45;g+=P(`M${x} ${y+h}L${tx-10} ${ty+10}Q${tx} ${ty} ${tx+12} ${ty-4}L${x+w} ${y+h*0.33}`,{s:C.val,sw:2});
  g+=CI(tx,ty+1,4.5,C.acc,{s:C.well,sw:1.5});return g}
function filterd(x,y,w,h){const id='f'+(UID++);let g=`<clipPath id="${id}"><rect x="${x}" y="${y}" width="${w}" height="${h}" rx="5"/></clipPath><g clip-path="url(#${id})">`+R(x,y,w,h,C.well);
  for(let k=1;k<10;k++){const gx=x+w*Math.log10(1+k*1.9)/Math.log10(20);g+=L(gx,y,gx,y+h,'#ffffff',1,{op:0.04})}
  const f=0.62,mid=y+h*0.52;let d='';for(let k=0;k<=120;k++){const u=k/120;const v=0.55*Math.exp(-Math.pow((u-f)/0.035,2))-(u>f?(u-f)*4.2:0);d+=(k?'L':'M')+(x+w*u).toFixed(1)+' '+(mid-v*h*0.5).toFixed(1)}
  g+=P(d+`L${x+w} ${y+h}L${x} ${y+h}Z`,{f:C.val,op:0.14});g+=P(d,{s:C.val,sw:2});g+=CI(x+w*f,mid-0.55*h*0.5,5.5,C.val,{s:C.well,sw:1.5});return g+'</g>'}
function decayd(x,y,w,h){const id='r'+(UID++),r=rng(5);let g=`<clipPath id="${id}"><rect x="${x}" y="${y}" width="${w}" height="${h}" rx="5"/></clipPath><g clip-path="url(#${id})">`+R(x,y,w,h,C.well);
  for(let k=0;k<Math.floor(w/3);k++){const u=k/(w/3);const e=Math.exp(-u*3.4)*(0.55+0.45*r())*(u<0.04?0.2:1);g+=L(x+k*3+1,y+h-2,x+k*3+1,y+h-2-e*(h-10),u<0.12?C.acc:C.val,1.6,{op:0.85})}
  let d=`M${x} ${y+8}`;for(let k=0;k<=60;k++){const u=k/60;d+=`L${(x+w*u).toFixed(1)} ${(y+h-2-Math.exp(-u*3.4)*(h-10)).toFixed(1)}`}g+=P(d,{s:'#ffffff',sw:1,op:0.5});return g+'</g>'}
// A scrolling spectrogram (time against frequency), drawn into a canvas and placed as a picture.
function spectro(x,y,w,h,seed){const cv=document.createElement('canvas');cv.width=w;cv.height=h;const c=cv.getContext('2d'),im=c.createImageData(w,h),r=rng(seed);
  const pal=v=>{v=Math.max(0,Math.min(1,v));const st=[[8,8,14],[40,18,84],[130,32,110],[224,84,60],[250,170,50],[255,244,190]];const p=v*(st.length-1),i=Math.min(st.length-2,Math.floor(p)),t=p-i;return st[i].map((a,k)=>a+(st[i+1][k]-a)*t)};
  const notes=[];for(let k=0;k<26;k++)notes.push({t0:r()*w,len:20+r()*120,f:0.12+r()*0.5,a:0.5+0.5*r(),harm:2+Math.floor(r()*6)});
  for(let px=0;px<w;px++){const beat=(px%46)<3?1:0,hat=(px%23)<2?1:0;for(let py=0;py<h;py++){const u=1-py/h;let v=0.10*(1-u)+0.05*r();
      v+=beat*(u<0.22?0.9*(1-u*3):0.25*(1-u));v+=hat*(u>0.62?0.5:0);
      for(const n of notes){if(px<n.t0||px>n.t0+n.len)continue;const env=Math.exp(-(px-n.t0)/n.len*1.8);for(let k=1;k<=n.harm;k++){const d=Math.abs(u-n.f*(0.35+0.21*k));if(d<0.006)v+=n.a*env/k*1.1}}
      const[cr,cg,cb]=pal(v);const o=(py*w+px)*4;im.data[o]=cr;im.data[o+1]=cg;im.data[o+2]=cb;im.data[o+3]=255}}
  c.putImageData(im,0,0);const id='s'+(UID++);return`<clipPath id="${id}"><rect x="${x}" y="${y}" width="${w}" height="${h}" rx="5"/></clipPath><image href="${cv.toDataURL()}" x="${x}" y="${y}" width="${w}" height="${h}" clip-path="url(#${id})"/>`}
// Device panels.
function panel(x,y,w,h,name,sub,o={}){let g=R(x,y,w,h,C.panel,{r:6,s:o.hot?C.acc:'#404044',sw:o.hot?1.5:1});g+=R(x+1,y+1,w-2,21,C.head,{r:5});g+=R(x+1,y+12,w-2,10,C.head);g+=iPower(x+13,y+12,C.acc);g+=T(x+26,y+15.5,name,{z:12,w:600});if(sub)g+=T(x+w-10,y+15.5,sub,{z:10.5,f:C.dim,a:'end'});return g}
function compPanel(x,y,w,h,k,o={}){let g=panel(x,y,w,h,'Compressor','Vocal ▾',o);const gh=Math.round(h*0.36),cw=gh;g+=curved(x+8,y+30,cw,gh);g+=dynd(x+14+cw,y+30,w-cw-36,gh,91+w);
  g+=meterV(x+w-18,y+30,4,gh,0.7)+R(x+w-11,y+30,4,gh,'#0d0d0f',{r:2})+R(x+w-11,y+30,4,gh*0.3,C.rec,{r:2});
  const ky=y+30+gh+(k>1?34:24),cols=5,cwid=(w-16)/cols;[['Thresh','-18.0 dB',0.4,C.val],['Ratio','4 : 1',0.5,C.val],['Attack','12 ms',0.3,C.val],['Release','160 ms',0.55,C.val],['Makeup','+3.0',0.6,C.acc]].forEach(([l,v,pv,c],n)=>g+=knob(x+8+cwid*(n+0.5),ky,k>1?16:11,pv,c,l,v,{lz:k>1?11:9.5}));
  const py=y+h-(k>1?30:24);g+=pill(x+10,py,k>1?92:80,k>1?19:16,'Key: Internal');g+=pill(x+(k>1?110:96),py,k>1?52:44,k>1?19:16,'Listen');return g}
function limPanel(x,y,w,h,k,o={}){let g=panel(x,y,w,h,'Limiter','Transparent ▾',o);const gh=Math.round(h*0.36);g+=dynd(x+8,y+30,w-40,gh,57+w,{per:5,depth:0.3,col:'#9fd0ff',noth:1});
  g+=meterV(x+w-27,y+30,7,gh,0.86,0.95)+meterV(x+w-17,y+30,7,gh,0.84,0.95);
  const ky=y+30+gh+(k>1?34:24),cols=4,cwid=(w-16)/cols;[['Gain','+4.8 dB',0.58,C.acc],['Attack','275 ms',0.45,C.val],['Release','400 ms',0.5,C.val],['Output','-0.30',0.9,C.val]].forEach(([l,v,pv,c],n)=>g+=knob(x+8+cwid*(n+0.5),ky,k>1?16:11,pv,c,l,v,{lz:k>1?11:9.5}));
  const py=y+h-(k>1?30:24);g+=pill(x+10,py,40,k>1?19:16,'Clip');g+=pill(x+56,py,66,k>1?19:16,'True Peak',{on:1});g+=T(x+w-10,py+12,'-0.3 TP',{z:10,f:C.dim,a:'end'});return g}
const BANDS=[{t:'hp',f:0.13,c:'#ffad3b'},{f:0.34,g:-0.4,q:0.05,c:'#c479ff'},{f:0.6,g:0.5,q:0.07,c:'#62d36b'},{t:'hs',f:0.84,g:0.45,c:'#4fc3ea'}];
function eqPanel(x,y,w,h,o={}){let g=panel(x,y,w,h,'EQ','4 bands  ·  Zero latency',o);g+=eqd(x+8,y+30,w-16,h-78,o.bands||BANDS,o.seed||41);
  ['20','50','100','200','500','1k','2k','5k','10k','20k'].forEach((t,k)=>g+=T(x+16+k*(w-40)/9,y+h-52,t,{z:8.5,f:C.faint}));
  const py=y+h-40;g+=pill(x+10,py,58,16,'Band 3',{on:1,c:'#62d36b'});g+=pill(x+74,py,48,16,'Bell ▾');g+=pill(x+128,py,74,16,'12 dB/oct ▾');g+=pill(x+208,py,58,16,'Stereo ▾');if(w>360)g+=pill(x+272,py,62,16,'Dynamic');if(w>520)g+=pill(x+340,py,92,16,'Unmask from ▾');
  g+=T(x+10,y+h-8,'2.84 kHz   +4.1 dB   Q 1.20',{z:10.5,f:C.dim});g+=T(x+w-10,y+h-8,'Analyzer  Pre + Post ▾',{z:10.5,f:C.dim,a:'end'});return g}
// A level meter in the manner of a mastering meter: Peak and RMS pairs, dynamic range, correlation, balance.
function bigMeter(x,y,w,h,title){let g=R(x,y,w,h,C.panel,{r:6});g+=T(x+12,y+18,title||'Meter',{z:12,w:600});g+=T(x+w-12,y+18,'Main ▾',{z:10.5,f:C.dim,a:'end'});
  const top=y+58,bot=y+h-176,mh=bot-top,bw=14;const cols=[x+16,x+34,x+w-48,x+w-30];
  g+=T((cols[0]+cols[1]+bw)/2,y+36,'Peak',{z:10.5,f:C.dim,a:'middle'});g+=T((cols[2]+cols[3]+bw)/2,y+36,'RMS',{z:10.5,f:C.dim,a:'middle'});
  ['L','R','L','R'].forEach((t,k)=>g+=T(cols[k]+bw/2,y+50,t,{z:9.5,f:C.faint,a:'middle'}));
  [0.84,0.8,0.66,0.63].forEach((v,k)=>g+=meterV(cols[k],top,bw,mh,v,Math.min(0.98,v+0.08)));
  const sx=(cols[1]+bw+cols[2])/2;['0','-3','-6','-10','-15','-20','-25','-30','-40','-60'].forEach((t,k,a)=>{const yy=top+mh*k/(a.length-1);g+=T(sx,yy+3.5,t,{z:9.5,f:C.dim,a:'middle'});g+=L(cols[1]+bw+3,yy,cols[1]+bw+7,yy,'#555')+L(cols[2]-7,yy,cols[2]-3,yy,'#555')});
  ['-1.2','-1.6','-9.8','-10.4'].forEach((t,k)=>g+=box(cols[k]-5+(k%2?4:-4)+(k>1?0:0),bot+8,26,16,t,{z:9}));
  let yy=bot+44;g+=T(x+12,yy,'Dynamic Range',{z:10.5,f:C.dim});g+=T(x+w-12,yy,'8.5 dB',{z:11,a:'end',w:600});g+=R(x+12,yy+7,w-24,6,'#0d0d0f',{r:3});g+=R(x+12,yy+7,(w-24)*0.42,6,C.grn,{r:3});
  yy+=40;g+=T(x+12,yy,'Correlation',{z:10.5,f:C.dim});g+=T(x+w-12,yy,'+0.62',{z:11,a:'end',w:600});g+=R(x+12,yy+7,w-24,6,'#0d0d0f',{r:3});g+=R(x+w/2,yy+7,(w-24)*0.31,6,C.grn,{r:3});g+=L(x+w/2,yy+5,x+w/2,yy+15,'#777');g+=T(x+12,yy+25,'-1',{z:8.5,f:C.faint});g+=T(x+w-12,yy+25,'+1',{z:8.5,f:C.faint,a:'end'});
  yy+=46;g+=T(x+12,yy,'Balance',{z:10.5,f:C.dim});g+=T(x+w-12,yy,'2 R',{z:11,a:'end',w:600});g+=R(x+12,yy+7,w-24,6,'#0d0d0f',{r:3});g+=R(x+w/2,yy+7,8,6,C.val,{r:3});g+=L(x+w/2,yy+5,x+w/2,yy+15,'#777');g+=T(x+12,yy+25,'L',{z:8.5,f:C.faint});g+=T(x+w-12,yy+25,'R',{z:8.5,f:C.faint,a:'end'});
  return g}
// The Info box: the default analyser is the spectrogram.
function infoBox(x,y,w,h,o={}){let g=R(x,y,w,h,C.panel,{r:6});g+=T(x+10,y+17,'Spectrogram',{z:11.5,w:600});g+=T(x+w-10,y+17,(o.src||'Main')+' ▾',{z:10.5,f:C.dim,a:'end'});
  const ix=x+34,iy=y+26,iw=w-42,ih=h-50;g+=spectro(ix,iy,iw,ih,o.seed||3);['20k','5k','1k','200','50'].forEach((t,k)=>g+=T(x+30,iy+8+k*(ih-10)/4,t,{z:8.5,f:C.faint,a:'end'}));
  g+=T(x+10,y+h-8,'Enhanced  ·  Log  ·  4096',{z:9.5,f:C.faint});g+=T(x+w-10,y+h-8,'Click: the analysers',{z:9.5,f:C.dim,a:'end'});return g}
function finish(W,H,s){document.getElementById('o').innerHTML=`<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}">${DEFS()}${s}</svg>`}
