// Second pass (17 November, evening): the spectrogram redrawn after the director's reference, a spectrum
// view to switch to, and the meter laid out and read as the mastering meter he named. Loaded after lib.js;
// these definitions replace the ones there. ADI's own drawing: no other maker's bitmaps, logo or font.

// A reassigned-looking spectrogram: thin partials that bend with the pitch, fine dots, haze at the top.
function spectro(x,y,w,h,seed){const cv=document.createElement('canvas');cv.width=w;cv.height=h;const c=cv.getContext('2d'),im=c.createImageData(w,h),r=rng(seed),buf=new Float32Array(w*h);
  const fy=f=>h*(1-Math.log(f/45)/Math.log(20000/45));const add=(px,py,v)=>{px|=0;py|=0;if(px>=0&&px<w&&py>=0&&py<h)buf[py*w+px]+=v};
  // haze: broad, soft energy in the upper part, moving slowly
  let hz=0.4;for(let px=0;px<w;px++){hz=hz*0.9+0.1*r();for(let py=0;py<h*0.62;py++){const u=1-py/(h*0.62);buf[py*w+px]+=(0.06+0.40*hz)*u*u*(0.7+0.3*r())}}
  // phrases: a fundamental that glides and wobbles, with many partials
  let t=4;while(t<w-8){const len=26+Math.floor(r()*70),base=150+r()*190,g0=(r()-0.5)*1.1,vib=0.006+r()*0.03,ph=r()*6.28,form=600+r()*1800;
    for(let k=0;k<len&&t+k<w;k++){const u=k/len,env=Math.min(1,u*8)*Math.min(1,(1-u)*5),f0=base*(1+g0*(u-0.5)*(u-0.5)*4-g0*0.5)*(1+vib*Math.sin(ph+k*0.55));
      for(let n=1;n<=60;n++){const f=n*f0;if(f>19000)break;const fm=0.35+0.65*Math.exp(-Math.pow((f-form)/(form*0.9),2))+(n<3?0.6:0);const a=env*fm*(n<4?1.25:1/Math.pow(n,0.32));
        const yy=fy(f)+(r()-0.5)*(n>12?1.6:0.5);if(r()<0.9)add(t+k,yy,a);if(r()<0.35)add(t+k,yy+1,a*0.45)}}
    // a burst of noise where the phrase begins
    const bx=t;for(let py=0;py<h*0.7;py++)if(r()<0.5)add(bx+(r()<0.5?0:1),py,0.25+0.35*r());
    t+=len+3+Math.floor(r()*16)}
  // fine dots everywhere, thinning towards the bottom
  for(let k=0;k<w*h*0.035;k++){const px=r()*w,py=r()*h;add(px,py,0.12+0.30*r()*(1-py/h*0.5))}
  const st=[[0,0,0],[30,12,58],[92,26,110],[176,44,104],[232,96,52],[248,170,48],[255,240,170]];
  for(let i=0;i<w*h;i++){let v=Math.max(0,Math.min(1,buf[i]*0.82));v=Math.pow(v,0.85);const p=v*(st.length-1),a=Math.min(st.length-2,Math.floor(p)),tt=p-a,o=i*4;
    im.data[o]=st[a][0]+(st[a+1][0]-st[a][0])*tt;im.data[o+1]=st[a][1]+(st[a+1][1]-st[a][1])*tt;im.data[o+2]=st[a][2]+(st[a+1][2]-st[a][2])*tt;im.data[o+3]=255}
  c.putImageData(im,0,0);const id='s'+(UID++);return`<clipPath id="${id}"><rect x="${x}" y="${y}" width="${w}" height="${h}" rx="4"/></clipPath><image href="${cv.toDataURL()}" x="${x}" y="${y}" width="${w}" height="${h}" clip-path="url(#${id})"/>`}
// A spectrum view: the live curve filled, a slower maximum line over it, decibel and frequency grids.
function spectrumd(x,y,w,h,seed){const id='p'+(UID++),r=rng(seed);let g=`<clipPath id="${id}"><rect x="${x}" y="${y}" width="${w}" height="${h}" rx="4"/></clipPath><g clip-path="url(#${id})">`+R(x,y,w,h,'#0c0d10');
  for(let k=1;k<6;k++)g+=L(x,y+h*k/6,x+w,y+h*k/6,'#ffffff',1,{op:0.06});[20,50,100,200,500,1000,2000,5000,10000].forEach(f=>{const gx=x+w*Math.log(f/20)/Math.log(1000);g+=L(gx,y,gx,y+h,'#ffffff',1,{op:0.06})});
  let d='',mx='',lv=0.5,n=Math.floor(w/1.6);for(let k=0;k<=n;k++){const u=k/n;lv=lv*0.55+0.45*r();const env=0.30+0.50*Math.exp(-Math.pow((u-0.22)/0.20,2))+0.28*Math.exp(-Math.pow((u-0.55)/0.22,2))-0.5*Math.max(0,u-0.82)*2.2;
    const v=Math.max(0.02,env*(0.72+0.42*lv)),m=Math.max(0.03,env*1.12+0.03);d+=(k?'L':'M')+(x+w*u).toFixed(1)+' '+(y+h-h*0.86*v).toFixed(1);mx+=(k?'L':'M')+(x+w*u).toFixed(1)+' '+(y+h-h*0.86*m).toFixed(1)}
  g+=P(d+`L${x+w} ${y+h}L${x} ${y+h}Z`,{f:'#b8d14a',op:0.30});g+=P(d,{s:'#d6ea6a',sw:1.1});g+=P(mx,{s:'#ff8a4a',sw:1,op:0.85});return g+'</g>'}
// The Info box. Its analyser is switched in its title: the spectrogram is the default, the spectrum the other.
function infoBox(x,y,w,h,o={}){const spec=o.mode==='spectrum';let g=R(x,y,w,h,C.panel,{r:6});
  g+=grp(x+6,y+5,168,20);g+=R(x+8+(spec?86:0),y+7,spec?78:84,16,C.acc,{r:5});g+=T(x+50,y+19,'Spectrogram',{z:10.5,a:'middle',w:700,f:spec?C.dim:C.ink});g+=T(x+133,y+19,'Spectrum',{z:10.5,a:'middle',w:700,f:spec?C.ink:C.dim});
  g+=T(x+w-10,y+19,(o.src||'Main')+' ▾',{z:10.5,f:C.dim,a:'end'});
  const ix=x+30,iy=y+32,iw=w-38,ih=h-54;
  if(spec){g+=spectrumd(ix,iy,iw,ih,o.seed||3);['0','-18','-36','-54','-72'].forEach((t,k)=>g+=T(x+26,iy+8+k*(ih-10)/4,t,{z:8.5,f:C.faint,a:'end'}));['20','100','1k','10k'].forEach((t,k)=>g+=T(ix+iw*[0,0.233,0.566,0.9][k]+3,iy+ih-4,t,{z:8.5,f:C.dim}))}
  else{g+=spectro(ix,iy,iw,ih,o.seed||3);['20k','5k','1k','200','50'].forEach((t,k)=>g+=T(x+26,iy+8+k*(ih-10)/4,t,{z:8.5,f:C.faint,a:'end'}))}
  g+=T(x+10,y+h-7,spec?'Stereo  ·  4096  ·  Slope 4.5':'Enhanced  ·  Log  ·  4096',{z:9.5,f:C.faint});g+=T(x+w-10,y+h-7,'Click: the Analysers',{z:9.5,f:C.dim,a:'end'});return g}
// The meter, laid out and read as the mastering meter the director named: Balance and Correlation at the
// head; the numbers for Peak, Dynamic and RMS; LEFT and RIGHT bars showing peak with RMS inside and their
// hold marks; the two floating Dynamic Range bars between them; scales at both sides; Float; Reset, the
// weighting and the mode; Solo L, R, Filter, M, S.
function bigMeter(x,y,w,h,title){let g=R(x,y,w,h,'#17171a',{r:6,s:'#2c2c33'});g+=T(x+w/2,y+18,title||'Meter',{z:12,w:700,a:'middle'});
  const leds=(lx,ly,lw,v,bi)=>{let q='';const n=25;for(let k=0;k<n;k++){const u=k/(n-1),on=bi?(v>=0.5?(u>=0.5&&u<=v):(u<=0.5&&u>=v)):u<=v,mid=Math.abs(u-0.5)<0.03;q+=R(lx+k*lw/n,ly,lw/n-1.2,7,on?(mid?'#f0f0f0':C.val):'#0a0a0c',{r:1})}return q};
  let yy=y+30;g+=T(x+10,yy+7,'12L',{z:8.5,f:C.dim});g+=T(x+w/2,yy+7,'Balance',{z:9,f:C.text,a:'middle',ls:0.5});g+=T(x+w-10,yy+7,'12R',{z:8.5,f:C.dim,a:'end'});g+=leds(x+10,yy+11,w-20,0.54,1);
  yy+=26;g+=T(x+10,yy+7,'-1',{z:8.5,f:C.dim});g+=T(x+w/2,yy+7,'Correlation',{z:9,f:C.text,a:'middle',ls:0.5});g+=T(x+w-10,yy+7,'+1',{z:8.5,f:C.dim,a:'end'});g+=leds(x+10,yy+11,w-20,0.82,1);
  // the numbers
  yy+=30;const nb=(bx,by,t,c)=>R(bx,by,30,14,'#0a0a0c',{r:2,s:'#34343c'})+T(bx+15,by+10.5,t,{z:9,a:'middle',f:c||C.text});
  g+=T(x+8,yy+10.5,'Peak',{z:8.5,f:C.dim});g+=nb(x+34,yy,'-2.3');g+=T(x+w/2,yy+10.5,'Dynamic',{z:8.5,f:C.dim,a:'middle'});g+=nb(x+w-64,yy,'-3.4');g+=T(x+w-8,yy+10.5,'Peak',{z:8.5,f:C.dim,a:'end'});
  yy+=18;g+=T(x+8,yy+10.5,'Rms',{z:8.5,f:C.dim});g+=nb(x+34,yy,'-9.9');g+=nb(x+w/2-31,yy,'6.1',C.val);g+=nb(x+w/2+1,yy,'5.8',C.val);g+=nb(x+w-64,yy,'-10.7');g+=T(x+w-8,yy+10.5,'Rms',{z:8.5,f:C.dim,a:'end'});
  // the bars
  const top=yy+30,bot=y+h-118,mh=bot-top,bw=15,lx=x+34,rx=x+w-34-bw;const marks=['0','-3','-6','-9','-12','-15','-18','-21','-24','-27','-30','-33','-36','-39','-42','-45','-48'];
  marks.forEach((t,k)=>{const my=top+mh*k/(marks.length-1);g+=T(x+26,my+3,t,{z:8,f:C.dim,a:'end'});g+=T(x+w-26,my+3,t,{z:8,f:C.dim});g+=L(x+28,my,x+31,my,'#55555f');g+=L(x+w-31,my,x+w-28,my,'#55555f')});
  const bar=(bx,pk,rm,hold)=>{let q=R(bx-1,top-1,bw+2,mh+2,'#050506',{r:2,s:'#30303a'});const seg=3,n=Math.floor(mh/seg);for(let k=0;k<n;k++){const u=1-k/n,sy=top+k*seg;let c='#0d0d10';if(u<=rm)c=u>0.9?'#ff5a4a':u>0.8?'#f2c53d':'#62d36b';else if(u<=pk)c=u>0.9?'#8a2f28':u>0.8?'#7d6820':'#2f6a38';q+=R(bx,sy,bw,seg-1,c)}
    q+=R(bx-1,top+mh*(1-hold),bw+2,2,'#f3f3f3');q+=R(bx-1,top+mh*(1-rm-0.03),bw+2,1.5,C.acc);return q};
  g+=bar(lx,0.86,0.66,0.93);g+=bar(rx,0.82,0.63,0.9);
  // the floating Dynamic Range bars between them, with their own small scale
  const dx=x+w/2,dw=9;[[dx-dw-7,0.86,0.66],[dx+7,0.82,0.63]].forEach(([bx,pk,rm])=>{g+=R(bx-1,top-1,dw+2,mh+2,'#050506',{r:2,s:'#26262d'});const y0=top+mh*(1-pk),y1=top+mh*(1-rm);for(let sy=y0;sy<y1;sy+=3)g+=R(bx,sy,dw,2,C.val)});
  ['3','6','9','12'].forEach((t,k)=>{const my=top+mh*(1-0.86)+ (k+1)*mh*0.052;g+=T(dx,my+3,t,{z:7.5,f:C.dim,a:'middle'})});
  // the foot
  let fy=bot+12;g+=T(lx+bw/2,fy+8,'LEFT',{z:8.5,f:C.dim,a:'middle',ls:0.5});g+=T(rx+bw/2,fy+8,'RIGHT',{z:8.5,f:C.dim,a:'middle',ls:0.5});g+=R(dx-26,fy,12,10,C.val,{r:2});g+=T(dx-10,fy+8.5,'Float',{z:8.5,f:C.text});
  fy+=20;const bwid=(w-16-8)/3;['Reset','Filter C ▾','Stereo ▾'].forEach((t,k)=>g+=btn(x+8+k*(bwid+4),fy,bwid,18,t,{z:9,r:4,w:500}));
  fy+=28;g+=T(x+w/2,fy+6,'Solo',{z:8.5,f:C.dim,a:'middle',ls:0.5});fy+=12;const sw=(w-16-16)/5;['L','R','Filter','M','S'].forEach((t,k)=>{g+=R(x+8+k*(sw+4)+sw/2-7,fy,14,12,'#0a0a0c',{r:2,s:'#3a3a45'});g+=T(x+8+k*(sw+4)+sw/2,fy+25,t,{z:8.5,f:C.dim,a:'middle'})});
  return g}
