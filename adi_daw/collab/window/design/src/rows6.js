// The Mix View's rows, proposal 6 (17 November, night). No channel block at the right: a row's name is a
// small tag at its left with the channel's colour, Mute and Solo two small buttons over it. Larger small
// rows, so that every EQ reads clearly down the list: a grid, a thin spectrum, one clean curve. Knobs show
// at every size. A solid "+" ends every chain; its menu adds a Mix View tool, a device or a plug-in.
// Uses ax, ar, rows, bandsets, SB and s from the page.
const FOC=5,GUT=112,HT=[56,82,122,250],EQW=[300,340,400,570],DYW=[214,244,284,296],PLW=[130,140,160,180];
const lvl=i=>{const d=Math.abs(i-FOC);return d===0?3:d===1?2:d===2?1:0};
function card(x,y,w,h,label,hot){let g=R(x,y,w,h,hot?'#1f1f28':'#18181d',{r:8,s:hot?'#5c5c6c':'#23232b'});g+=`<text transform="translate(${x+11} ${y+h/2}) rotate(-90)" font-size="${h<60?7.5:h<130?8.5:9.5}" fill="${hot?C.dim:C.faint}" text-anchor="middle" letter-spacing="1.2">${label}</text>`;return g}
// The EQ's display: frequency grid, a thin spectrum, the curve with a soft glow; bands show when it is large.
function eqf(x,y,w,h,bands,seed,lv){const r=rng(seed),id='q'+(UID++),mid=y+h*0.5,amp=h*0.30;let g=`<clipPath id="${id}"><rect x="${x}" y="${y}" width="${w}" height="${h}" rx="6"/></clipPath><g clip-path="url(#${id})">`+R(x,y,w,h,'#121216');
  const fx=f=>x+w*Math.log(f/20)/Math.log(1000);[30,40,50,60,70,80,90,200,300,400,500,600,700,800,900,2000,3000,4000,5000,6000,7000,8000,9000].forEach(f=>g+=L(fx(f),y,fx(f),y+h,'#ffffff',1,{op:0.028}));[100,1000,10000].forEach(f=>g+=L(fx(f),y,fx(f),y+h,'#ffffff',1,{op:0.075}));
  g+=L(x,mid,x+w,mid,'#ffffff',1,{op:0.10});if(lv>1){g+=L(x,mid-amp,x+w,mid-amp,'#ffffff',1,{op:0.04});g+=L(x,mid+amp,x+w,mid+amp,'#ffffff',1,{op:0.04})}
  const one=(b,u)=>b.t==='hp'?-2.2*Math.pow(Math.max(0,(b.f-u)/0.13),1.4):b.t==='hs'?b.g/(1+Math.exp((b.f-u)/0.045)):b.t==='ls'?b.g/(1+Math.exp((u-b.f)/0.045)):b.g*Math.exp(-Math.pow((u-b.f)/(b.q||0.07),2));
  const resp=u=>bands.reduce((v,b)=>v+one(b,u),0),Y=u=>Math.max(y-6,Math.min(y+h+6,mid-resp(u)*amp));
  // the spectrum: smooth, falling to the right, following the curve a little
  let sp='',n=150,a=0.5,b2=0.5;for(let k=0;k<=n;k++){const u=k/n;a=a*0.55+0.45*r();b2=b2*0.8+0.2*a;const env=(0.30+0.62*Math.exp(-Math.pow((u-0.30)/0.26,2))+0.30*Math.exp(-Math.pow((u-0.62)/0.2,2)))*(u>0.86?Math.max(0,1-(u-0.86)*6.5):1)*(u<0.1?u*10:1);
    const v=Math.max(0,env*(0.62+0.5*b2)+resp(u)*0.16);sp+=(k?'L':'M')+(x+w*u).toFixed(1)+' '+(y+h-v*h*0.62).toFixed(1)}
  g+=P(sp+`L${x+w} ${y+h}L${x} ${y+h}Z`,{f:'#c9c9d6',op:0.07});g+=P(sp,{s:'#a9a9b6',sw:lv>1?1:0.8,op:0.6});
  // the low cut's shade, as light from the left
  const hp=bands.find(b=>b.t==='hp');const m=160;let d='';for(let k=0;k<=m;k++){const u=k/m;d+=(k?'L':'M')+(x+w*u).toFixed(1)+' '+Y(u).toFixed(1)}
  if(hp){let sh=`M${x} ${mid}`;for(let k=0;k<=m;k++){const u=k/m;if(u>hp.f+0.03)break;sh+=`L${(x+w*u).toFixed(1)} ${Y(u).toFixed(1)}`}sh+=`L${(x+w*(hp.f+0.03)).toFixed(1)} ${mid}Z`;g+=P(sh,{f:C.yel,op:0.13})}
  if(lv>1)for(const bd of bands){if(bd.t==='hp')continue;let p=`M${x} ${mid}`;for(let k=0;k<=m;k++){const u=k/m;p+=`L${(x+w*u).toFixed(1)} ${(mid-one(bd,u)*amp).toFixed(1)}`}g+=P(p+`L${x+w} ${mid}Z`,{f:bd.c,op:0.20})}
  g+=P(d,{s:C.yel,sw:lv>1?5:4,op:0.16});g+=P(d,{s:C.yel,sw:lv>2?2.1:lv>1?1.8:1.5});
  if(lv>1)for(const bd of bands){const c=bd.c||C.yel;g+=CI((x+w*bd.f).toFixed(1),Math.max(y+7,Math.min(y+h-7,Y(bd.f))).toFixed(1),lv===3?6:4,c,{s:'#121216',sw:1.5})}
  return g+'</g>'}
function eqCard(x,y,w,h,lv,bands,seed,hot){let g=card(x,y,w,h,'EQ',hot);const dx=x+20,dw=w-26,dy=y+(lv>1?6:4),dh=h-(lv>1?12:8);g+=eqf(dx,dy,dw,dh,bands,seed,lv);
  if(lv===3){g+=iPower(dx+12,dy+13,C.dim);g+=T(dx+dw-10,dy+16,'12 dB',{z:10.5,f:C.acc,a:'end'});[['20',20],['50',50],['100',100],['200',200],['500',500],['1k',1000],['2k',2000],['5k',5000],['10k',10000]].forEach(([t,f])=>g+=T(dx+dw*Math.log(f/20)/Math.log(1000)+3,dy+dh-5,t,{z:8.5,f:C.faint}))}
  return g}
function dynCard(x,y,w,h,lv,kind,seed,arg,per){const NAME={comp:'COMP',lim:'LIMIT',clip:'CLIP',unmask:'MASK'}[kind],col=kind==='unmask'?'#c479ff':kind==='lim'?'#9fd0ff':undefined,dep=kind==='lim'?0.26:kind==='unmask'?0.4:0.3;let g=card(x,y,w,h,NAME,false);
  const set=kind==='lim'?[['GAIN',0.58,C.acc],['ATTACK',0.45,C.val],['OUTPUT',0.9,C.val]]:kind==='clip'?[['DRIVE',0.5,C.acc],['CEILING',0.85,C.val],['SOFT',0.3,C.val]]:[['THRESHOLD',0.4,C.grn],['RATIO',0.5,C.acc],['GAIN',0.6,C.val]];
  if(lv<3){const kr=[9,11.5,14][lv],sp=[34,40,46][lv],kx=x+26+kr,ky=y+h/2-(lv?7:5);set.forEach(([l,v,c],k)=>g+=knob(kx+k*sp,ky,kr,v,c,l,'',{lz:[6.5,7.5,8.5][lv]}));
    const gx=kx+2*sp+kr+14;g+=dynd(gx,y+5,x+w-gx-16,h-10,seed,{mini:lv===0,per:per,depth:dep*(lv?1:0.8),col:col,noth:1});g+=meterV(x+w-12,y+5,4,h-10,kind==='lim'?0.86:0.7);if(arg)g+=T(x+w-20,y+16,'◂ '+arg,{z:9,f:C.acc,a:'end'});return g}
  const gx=x+w-128,gw=104;g+=dynd(gx,y+10,gw,h-20,seed,{per:per,depth:dep,col:col,noth:kind==='lim'});g+=meterV(x+w-18,y+10,5,h-20,kind==='lim'?0.86:0.7,kind==='lim'?0.95:0)+R(x+w-11,y+10,5,h-20,'#0d0d0f',{r:2})+R(x+w-11,y+10,5,(h-20)*0.26,C.rec,{r:2});
  if(kind==='lim'){g+=knob(x+62,y+66,22,0.58,C.acc,'GAIN','+4.8 dB',{lz:9.5});g+=knob(x+132,y+70,17,0.9,C.val,'OUTPUT','-0.30',{lz:9.5});g+=knob(x+52,y+160,13,0.45,C.val,'ATTACK','275 ms',{lz:9});g+=knob(x+106,y+160,13,0.5,C.val,'RELEASE','400 ms',{lz:9});g+=pill(x+30,y+h-30,96,18,'Transparent ▾');g+=pill(x+132,y+h-30,34,18,'TP',{on:1})}
  else{g+=knob(x+62,y+66,22,0.42,C.grn,'THRESHOLD','-18.0 dB',{lz:9.5});g+=knob(x+132,y+70,17,0.5,C.acc,'RATIO','4.0 : 1',{lz:9.5});g+=knob(x+46,y+160,13,0.3,C.val,'ATTACK','12 ms',{lz:9});g+=knob(x+96,y+160,13,0.55,C.val,'RELEASE','160 ms',{lz:9});g+=knob(x+146,y+160,13,0.6,C.val,'GAIN','+3.0',{lz:9});g+=pill(x+30,y+h-30,66,18,'Vocal ▾');g+=pill(x+102,y+h-30,62,18,'Key ▾')}
  return g}
// gentler settings, as a mix has them
const SETS=[
 [{t:'hp',f:0.10},{f:0.24,g:0.45,q:0.06,c:'#ff5a5a'},{f:0.58,g:-0.3,q:0.09,c:'#62d36b'},{t:'hs',f:0.8,g:0.25,c:'#5ec8ff'}],[{t:'hp',f:0.2},{f:0.34,g:0.35,q:0.06,c:'#ff5a5a'},{t:'hs',f:0.76,g:0.4,c:'#5ec8ff'}],
 [{t:'hp',f:0.36},{t:'hs',f:0.82,g:0.35,c:'#c479ff'}],[{f:0.22,g:0.25,q:0.09,c:'#ff5a5a'},{f:0.5,g:-0.25,q:0.07,c:'#62d36b'},{t:'hs',f:0.84,g:0.3,c:'#5ec8ff'}],
 [{t:'ls',f:0.2,g:0.4,c:'#ff5a5a'},{f:0.38,g:-0.45,q:0.045,c:'#c479ff'},{f:0.62,g:0.2,q:0.08,c:'#62d36b'}],
 [{t:'hp',f:0.17,c:'#ffb13b'},{f:0.36,g:-0.4,q:0.05,c:'#c479ff'},{f:0.62,g:0.5,q:0.075,c:'#62d36b'},{t:'hs',f:0.86,g:0.4,c:'#5ec8ff'}],
 [{t:'hp',f:0.24},{f:0.66,g:0.3,q:0.1,c:'#62d36b'}],[{t:'hp',f:0.14},{f:0.42,g:-0.4,q:0.06,c:'#c479ff'},{f:0.6,g:0.25,q:0.06,c:'#62d36b'},{t:'hs',f:0.82,g:0.4,c:'#ff5a5a'}],
 [{f:0.32,g:-0.25,q:0.1,c:'#ff5a5a'},{t:'hs',f:0.8,g:0.25,c:'#5ec8ff'}],[{t:'hp',f:0.2},{f:0.5,g:-0.2,q:0.12,c:'#62d36b'}],[{t:'hp',f:0.26},{t:'hs',f:0.7,g:-0.25,c:'#5ec8ff'}],
 [{t:'hp',f:0.24},{t:'hs',f:0.72,g:-0.35,c:'#5ec8ff'}],[{t:'hp',f:0.22},{f:0.5,g:0.2,q:0.2,c:'#62d36b'}],[{t:'ls',f:0.14,g:0.2,c:'#ff5a5a'},{f:0.4,g:-0.15,q:0.1,c:'#c479ff'},{t:'hs',f:0.86,g:0.25,c:'#5ec8ff'}]];
let PLUS=null;
s+=`<clipPath id="rowsclip"><rect x="${ax-6}" y="172" width="${ar-ax+12}" height="${SB-6-172}"/></clipPath><g clip-path="url(#rowsclip)">`;
{let y=176;rows.forEach((row,i)=>{const lv=lvl(i),h=HT[lv],foc=lv===3;
  if(foc)s+=R(ax,y,ar-ax,h,'#17171d',{r:9});
  // the row's tag at its left: level, Mute and Solo, then the name with the channel's colour
  const inGroup=row.g==='Drums'&&row.n!=='Drum Bus';if(inGroup)s+=R(ax,y-2,3,h+4,'#ffb13b');s+=R(ax+(inGroup?5:0),y+3,3,h-6,row.c,{r:1.5});
  s+=meterV(ax+12,y+5,3,h-10,row.m)+meterV(ax+17,y+5,3,h-10,row.m*0.93);
  const tx=ax+GUT-8,by=y+h-10,bs=foc?20:lv===2?17:14;s+=CI(tx-3,by-4,foc?4:3.2,row.c);s+=T(tx-12,by,row.n,{z:foc?13:lv===2?12:11,a:'end',w:foc?700:lv?600:500,f:foc?'#ffffff':lv?C.text:'#b4b4bf'});
  s+=btn(tx-2*bs-5,by-(foc?24:20)-bs+8,bs,bs-2,'M',{z:foc?10.5:8.5,r:4,f:C.dim});s+=btn(tx-bs-1,by-(foc?24:20)-bs+8,bs,bs-2,'S',{z:foc?10.5:8.5,r:4,f:C.dim});
  if(foc){s+=T(ax+28,y+22,'-6.1 dB',{z:10,f:C.dim});s+=T(ax+28,y+37,'8.5 DR',{z:10,f:C.dim});s+=note(ax+GUT-30,y+h-58,9)}
  // the tools, in chain order
  const pad=foc?8:4,gap=foc?10:6;let fx=ax+GUT;const cy=y+pad,ch=h-2*pad;
  row.f.forEach((f,k)=>{const[kind,arg]=f.split(':');
    if(kind==='eq'){const w=EQW[lv];s+=eqCard(fx,cy,w,ch,lv,SETS[i],100+i,foc);
      if(foc){const bx=fx+20+(w-26)*0.62,byy=cy+26;s+=`<g filter="url(#sh)">`+R(bx-92,byy,184,44,'#0f0f13',{r:7,s:'#4b4b5a'})+'</g>';s+=iPower(bx-76,byy+15,C.acc);s+=T(bx-62,byy+19,'2.84 kHz',{z:12,w:600});s+=T(bx+78,byy+19,'+4.1 dB',{z:12,a:'end'});s+=T(bx-76,byy+35,'Q 1.20   Bell   12 dB/oct   Stereo ▾',{z:10,f:C.dim});
        s+=P(`M${bx+3} ${byy+56}l0 17l4.5 -4l3 7l3 -1.3l-3 -7l6 -0.4z`,{f:'#ffffff',s:'#101013',sw:1});s+=note(fx+w/2,cy-1,8)}fx+=w+gap}
    else if(kind==='plug'){const w=PLW[lv];s+=R(fx,cy,w,ch,'#18181d',{r:8,s:'#30303a'});s+=T(fx+w/2,cy+ch/2+(lv?-2:4),arg,{z:lv>1?11.5:10.5,a:'middle',f:C.text});if(lv)s+=T(fx+w/2,cy+ch/2+14,'click: open',{z:9,a:'middle',f:C.faint});fx+=w+gap}
    else{const w=DYW[lv];s+=dynCard(fx,cy,w,ch,lv,kind,200+i*7+k,arg,6+((i*3+k)%7));fx+=w+gap}});
  // the "+" that ends every chain
  const pw=foc?46:lv===2?40:34;s+=R(fx,cy,pw,ch,'#1c1c23',{r:8,s:'#4a4a58'});s+=P(`M${fx+pw/2-7} ${cy+ch/2}h14M${fx+pw/2} ${cy+ch/2-7}v14`,{s:foc?C.acc:C.text,sw:2});
  if(foc){s+=note(ax-2,y+12,7);PLUS=[fx+pw,cy+ch]}
  y+=h+4});}
s+='</g>';s+=R(ar+1,176,3,SB-6-176,'#0b0b0d',{r:1.5});s+=R(ar+1,176,3,(SB-6-176)*0.62,'#3a3a45',{r:1.5});
// the menu of the "+", shown open here: Mix View tools first, then devices, then plug-ins
if(PLUS){const mw=236,mh=226,mx=Math.min(PLUS[0]-mw,ar-mw-6),my=PLUS[1]+6;let g=`<g filter="url(#sh)">`+R(mx,my,mw,mh,'#22222a',{r:9,s:'#55556a'})+'</g>';
  g+=R(mx+8,my+8,mw-16,24,'#131317',{r:5,s:'#34343f'});g+=T(mx+18,my+24,'Find a tool, device or plug-in',{z:11,f:C.faint});let yy=my+50;
  const sec=t=>{g+=T(mx+12,yy,t,{z:9.5,f:C.dim,ls:1});yy+=8};const it=(t,o={})=>{if(o.sel)g+=R(mx+5,yy,mw-10,19,'#3d3d4b',{r:5});g+=R(mx+13,yy+4.5,10,10,'none',{r:2.5,s:o.c||C.acc,sw:1.4});g+=T(mx+31,yy+14,t,{z:11.5,f:o.dim?C.faint:C.text});if(o.r)g+=T(mx+mw-12,yy+14,o.r,{z:10,f:C.faint,a:'end'});yy+=20};
  sec('MIX VIEW TOOLS');it('EQ',{dim:1,r:'in this chain'});it('Compressor',{dim:1,r:'in this chain'});it('Limiter',{dim:1,r:'in this chain'});it('Clip',{sel:1});it('Unmask');yy+=12;
  sec('DEVICES AND PLUG-INS');it('Auto Filter',{c:C.val,r:'Pd'});g+=T(mx+mw-12,my+mh-9,'All devices and plug-ins  ›',{z:10.5,f:C.acc,a:'end'});
  s+=g;s+=note(mx-2,my+12,13)}
