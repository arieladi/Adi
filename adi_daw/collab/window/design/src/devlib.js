// The built-in devices' cards (ADR-0243; agreed by the director on 28 November 2026 from the third devices
// sheet): depth and glow, a large main knob with its value inside, the signal behind every curve, joined
// buttons for a short list, meters on a card. Used after lib.js by the devices sheets from the fourth on.
// Colours: orange is a device's own controls and what is switched on; blue is what moves them (LFO,
// envelope, a delay's filter) and the pictures of the signal; a band has its band's colour.
const O='#ffa93a',B='#4fc3ea',G='#62d36b',V='#c479ff',Y='#f2c53d',INK='#16171a';
const DEVDEFS=`<defs>
<filter id="glow" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="4.5" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>
<filter id="glow2" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="2.4" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>
<filter id="csh" x="-5%" y="-5%" width="110%" height="116%"><feDropShadow dx="0" dy="8" stdDeviation="12" flood-color="#000" flood-opacity=".65"/></filter>
<filter id="ksh" x="-30%" y="-30%" width="160%" height="170%"><feDropShadow dx="0" dy="4" stdDeviation="5" flood-color="#000" flood-opacity=".7"/></filter>
<linearGradient id="card" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#2b2c31"/><stop offset="1" stop-color="#1c1d21"/></linearGradient>
<linearGradient id="hd" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3a3b41"/><stop offset="1" stop-color="#2c2d32"/></linearGradient>
<linearGradient id="scr" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#17181c"/><stop offset="1" stop-color="#0d0e11"/></linearGradient>
<radialGradient id="dl" cx="36%" cy="26%" r="85%"><stop offset="0" stop-color="#55565e"/><stop offset=".55" stop-color="#2d2e33"/><stop offset="1" stop-color="#191a1d"/></radialGradient>
<linearGradient id="fO" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${O}" stop-opacity=".42"/><stop offset="1" stop-color="${O}" stop-opacity=".02"/></linearGradient>
<linearGradient id="fB" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${B}" stop-opacity=".45"/><stop offset="1" stop-color="${B}" stop-opacity=".03"/></linearGradient>
<linearGradient id="fY" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f6d24a" stop-opacity=".55"/><stop offset="1" stop-color="#f6d24a" stop-opacity=".04"/></linearGradient>
<linearGradient id="fS" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#b8bcc6" stop-opacity=".34"/><stop offset="1" stop-color="#b8bcc6" stop-opacity=".03"/></linearGradient>
</defs>`;
const para=(x,y,lines,o={})=>lines.map((t,k)=>T(x,y+k*(o.lh||16),t,{z:o.z||12,f:o.f||'#c4c6cc',w:o.w||400})).join('');
// A card. The arrow is drawn only where the card does not show every control of its device.
const card=(x,y,w,h,name,o={})=>`<g filter="url(#csh)">`+R(x,y,w,h,'url(#card)',{r:11,s:'#3d3e44'})+`</g>`+
  `<path d="M${x+1} ${y+36}V${y+12}Q${x+1} ${y+1} ${x+12} ${y+1}H${x+w-12}Q${x+w-1} ${y+1} ${x+w-1} ${y+12}V${y+36}Z" fill="url(#hd)"/>`+L(x+1,y+36,x+w-1,y+36,'#17181b')+
  CI(x+20,y+18.5,6.5,'none',{s:O,sw:2})+L(x+20,y+10.5,x+20,y+17.5,O,2,{cap:'round'})+
  T(x+38,y+24,name,{z:15,w:600,f:'#eceef2'})+(o.sub?T(x+w-(o.open===false?16:46),y+23,o.sub,{z:10.5,f:'#8b8e97',a:'end'}):'')+
  (o.open===false?'':R(x+w-34,y+9,22,19,'#2a2b30',{r:5,s:'#4a4b52'})+P(`M${x+w-28} ${y+23}L${x+w-18} ${y+14}M${x+w-24} ${y+14}h6v6`,{s:'#d8dae0',sw:1.5}));
const ring=(cx,cy,r,a0,a1,col,sw,glow)=>`<path d="${arc(cx,cy,r,a0,a1)}" fill="none" stroke="${col}" stroke-width="${sw}" stroke-linecap="round"${glow?` filter="url(#${glow})"`:''}/>`;
// A knob: its name above; its value inside when the knob is large enough, else below.
function kb(cx,cy,r,v,col,label,val,o={}){const a0=-138,a1=138,av=a0+(a1-a0)*v,rw=Math.max(3.2,r*0.13);let g='';
  if(o.dis)col='#5a5c63';
  g+=ring(cx,cy,r,a0,a1,'#0b0c0e',rw+1.5);g+=ring(cx,cy,r,a0,a1,'#34353b',rw-0.5);
  if(o.bi)g+=ring(cx,cy,r,Math.min(0,av),Math.max(0,av)+0.01,col,rw,o.dis?null:(r>40?'glow':'glow2'));else g+=ring(cx,cy,r,a0,av+0.01,col,rw,o.dis?null:(r>40?'glow':'glow2'));
  if(o.mod)g+=`<path d="${arc(cx,cy,r+rw+4,a0+(a1-a0)*o.mod[0],a0+(a1-a0)*o.mod[1])}" fill="none" stroke="${B}" stroke-width="2" stroke-dasharray="3 3" stroke-linecap="round"/>`;
  g+=`<g filter="url(#ksh)">`+CI(cx,cy,r-rw-3,'url(#dl)',{s:'#0c0d0f',sw:1.2})+`</g>`+CI(cx,cy,r-rw-3.6,'none',{s:'#ffffff',sw:1,op:0.07});
  const[px,py]=pol(cx,cy,r-rw-5,av),[qx,qy]=pol(cx,cy,r-rw-5-Math.max(5,r*0.2),av);g+=L(qx,qy,px,py,col,Math.max(1.8,r*0.045),{cap:'round'});
  const inside=r>=26,vc=o.dis?'#7c7f88':'#f6f7f9';
  if(inside)g+=T(cx,cy+(o.vz||Math.max(11,r*0.3))*0.36,val,{z:o.vz||Math.max(11,r*0.3),w:700,a:'middle',f:vc});
  else g+=T(cx,cy+r+16,val,{z:o.vs||11,f:o.dis?'#7c7f88':'#e3e5ea',a:'middle',w:600});
  g+=T(cx,cy-r-(r>40?14:9),label,{z:r>40?12:(o.lz||10),f:'#9a9da6',a:'middle',w:600,ls:0.7});
  if(o.min)g+=T(cx-r*0.78,cy+r+10,o.min,{z:9.5,f:'#70737c',a:'middle'})+T(cx+r*0.78,cy+r+10,o.max,{z:9.5,f:'#70737c',a:'middle'});
  return g}
const pills=(x,y,w,h,labels,sel,o={})=>{let g=R(x,y,w,h,'#141518',{r:h/2,s:'#3c3d43'});const n=labels.length,cw=w/n;
  labels.forEach((t,k)=>{if(k===sel)g+=`<g filter="url(#glow2)">`+R(x+k*cw+2,y+2,cw-4,h-4,o.c||O,{r:(h-4)/2})+`</g>`;
   g+=T(x+k*cw+cw/2,y+h/2+(o.z||11)*0.36,t,{z:o.z||11,a:'middle',f:k===sel?INK:'#d0d2d8',w:k===sel?700:500})});return g};
const tog=(x,y,w,h,t,on,o={})=>(on?`<g filter="url(#glow2)">`+R(x,y,w,h,o.c||O,{r:h/2})+`</g>`:R(x,y,w,h,'#26272c',{r:h/2,s:'#45464d'}))+T(x+w/2,y+h/2+(o.z||11)*0.36,t,{z:o.z||11,a:'middle',f:on?INK:'#d0d2d8',w:on?700:500});
const vbox=(x,y,w,h,name,val)=>R(x,y,w,h,'#141518',{r:5,s:'#3c3d43'})+T(x+8,y+h/2+3.8,name,{z:10.5,f:'#9a9da6'})+T(x+w-8,y+h/2+3.8,val,{z:10.5,f:'#eceef2',a:'end',w:600});
function meter(x,y,h,a,b,o={}){let g='';[[x,a],[x+8,b]].forEach(m=>{g+=R(m[0],y,5,h,'#0a0b0d',{r:2})+`<rect x="${m[0]}" y="${(y+h-h*m[1]).toFixed(1)}" width="5" height="${(h*m[1]).toFixed(1)}" rx="2" fill="url(#mv)"/>`+R(m[0]-1,y+h-h*Math.min(0.97,m[1]+0.09),7,1.6,'#f4f4f4')});
  if(!o.nolabel)g+=T(x+2.5,y+h+12,'L',{z:8.5,f:'#70737c',a:'middle'})+T(x+10.5,y+h+12,'R',{z:8.5,f:'#70737c',a:'middle'});return g}
function screen(x,y,w,h,fn){const id='q'+(UID++);return `<clipPath id="${id}"><rect x="${x}" y="${y}" width="${w}" height="${h}" rx="7"/></clipPath>`+R(x,y,w,h,'url(#scr)',{r:7})+`<g clip-path="url(#${id})">`+fn()+'</g>'+R(x,y,w,h,'none',{r:7,s:'#34353b'})}
// 20 Hz to 20 kHz across the width, with a labelled dB scale
const grid=(x,y,w,h,o={})=>{let g='';[['20',0.0],['100',0.233],['1k',0.566],['10k',0.9]].concat(w<400?[]:[['20k',1.0]]).forEach(a=>{g+=L(x+w*a[1],y,x+w*a[1],y+h,'#ffffff',1,{op:0.06});
  g+=T(x+w*a[1]+(a[1]>0.95?-4:4),y+h-5,a[0],{z:9,f:'#6b6e77',a:a[1]>0.95?'end':'start'})});
  (o.db||['+12','0','-12','-24']).forEach((t,k,arr)=>{const yy=y+h*(k+1)/(arr.length+1);g+=L(x,yy,x+w,yy,'#ffffff',1,{op:0.05})+T(x+w-5,yy-3,t,{z:8.5,f:'#5d6068',a:'end'})});return g};
function spectrum(x,y,w,h,seed,fill,peak,tilt){const r=rng(seed);let d=`M${x} ${y+h}`;const n=Math.floor(w/2.5);
  for(let k=0;k<=n;k++){const u=k/n;const env=0.25+0.5*Math.exp(-Math.pow((u-(peak||0.28))*2.5,2))-(tilt==null?0.22:tilt)*u;const v=Math.max(0.02,env*(0.74+0.26*r()));d+=`L${(x+u*w).toFixed(1)} ${(y+h-v*h).toFixed(1)}`}
  return `<path d="${d}L${x+w} ${y+h}Z" fill="${fill}"/>`}
const curve=(d,col,fill,x,y,w,h)=>(fill?`<path d="${d}L${x+w} ${y+h}L${x} ${y+h}Z" fill="${fill}"/>`:'')+`<path d="${d}" fill="none" stroke="${col}" stroke-width="2.4" stroke-linejoin="round" filter="url(#glow2)"/>`;
const dot=(x,y,col)=>`<g filter="url(#glow2)">`+CI(x,y,6,col)+`</g>`+CI(x,y,6,'none',{s:'#0d0e11',sw:1.6})+CI(x,y,2,'#0d0e11');
const cap=(x,y,t,o={})=>T(x,y,t,{z:10,f:'#8b8e97',w:600,ls:0.9,a:o.a||'start'});
const vline=(x,y1,y2)=>L(x,y1,x,y2,'#3a3b41');
// a path from a function v(u), u from 0 to 1, v upward from the bottom of the box
const plot=(x,y,w,h,fn,n=160)=>{let d='';for(let k=0;k<=n;k++){const u=k/n;d+=(k?'L':'M')+(x+w*u).toFixed(1)+' '+(y+h-fn(u)*h).toFixed(1)}return d};
function sheet(W,H,s,title,sub,stamp){let head=R(0,0,W,H,'#0f1012')+T(20,30,title,{z:17,w:700})+T(20,48,sub,{z:11.5,f:C.dim})+R(W-204,12,190,22,C.acc,{r:5})+T(W-109,27,stamp,{z:11.5,w:700,a:'middle',f:C.ink});
  document.getElementById('o').innerHTML=`<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}">${DEFS()}${DEVDEFS}${head}${s}</svg>`}
