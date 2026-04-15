// kr106-controls.js — Control layout and drawing functions
// Depends on: kr106-params.js (P, paramValues, images)

let benderTriggered=false; // vertical LFO trigger state (shared with kr106-ui.js)
const images={};
const imageFiles={bg:'kr106_bg.png',sliderHandle:'kr106_slider.png',switch3:'kr106_sw3.png',switch2:'kr106_sw2.png',switch3h:'kr106_sw3h.png',switch2h:'kr106_sw2h.png',ledRed:'kr106_led.png',smallknob:'kr106_knob.png',benderGrad:'kr106_bender.png'};
function loadImages(){return Promise.all(Object.entries(imageFiles).map(([k,src])=>new Promise(r=>{const i=new Image();i.onload=()=>{images[k]=i;r()};i.onerror=()=>r();i.src=src})))}

const controls=[
[P.arpRate,'slider',227,33,0],[P.lfoRate,'slider',249,33,1],[P.lfoDelay,'slider',267,33,0],
[P.dcoLfo,'slider',313,33,1],[P.dcoPwm,'slider',331,33,0],
[P.dcoSub,'slider',427,33,1],[P.dcoNoise,'slider',445,33,0],[P.hpfFreq,'slider',470,33,0],
[P.vcfFreq,'slider',495,33,1],[P.vcfRes,'slider',513,33,0],
[P.vcfEnv,'slider',549,33,1],[P.vcfLfo,'slider',567,33,1],[P.vcfKbd,'slider',585,33,0],
[P.vcaLevel,'slider',634,33,0],
[P.envA,'slider',656,33,1],[P.envD,'slider',674,33,1],[P.envS,'slider',692,33,1],[P.envR,'slider',710,33,0],
[P.benderDco,'slider',34,147,1],[P.benderVcf,'slider',52,147,1],[P.benderLfo,'slider',70,147,0],
[P.arpMode,'switch3',175,46],[P.arpRange,'switch3',212,46],[P.lfoMode,'switch2',294,46],
[P.pwmMode,'switch3',351,46],[P.vcfEnvInv,'switch2',536,46],[P.vcaMode,'switch2',614,45],
[P.portaMode,'switch3',92,161],
[P.octTranspose,'hswitch3',389,84],[P.adsrMode,'hswitch2',680,84],
[P.tuning,'knob',40,64],[P.masterVol,'knob',30,118],[P.portaRate,'knob',66,118],
[-1,'chorusoff',735,52],
[P.power,'power',46,40],
[-2,'clipled',53,127],
[P.bender,'bender',66,200],
[P.transpose,'btnled',95,43,1],[P.hold,'btnled',122,43,1],[P.arpeggio,'btnled',154,43,1],
[P.dcoPulse,'btnled',377,43,1],[P.dcoSaw,'btnled',393,43,1],[P.dcoSubSw,'btnled',409,43,2],
[P.chorusI,'btnled',751,43,1],[P.chorusII,'btnled',767,43,2],
[-1,'gear',751,75]];

function ctrlBounds(c){const[,t,x,y,e]=c;
if(t==='slider')return{x,y,w:19+(e||0),h:49};if(t==='switch3'||t==='switch2')return{x,y,w:9,h:24};
if(t==='hswitch3'||t==='hswitch2')return{x,y,w:24,h:9};if(t==='knob')return{x,y,w:27,h:27};
if(t==='chorusoff')return{x,y,w:17,h:19};if(t==='power')return{x,y,w:15,h:19};
if(t==='clipled')return{x,y,w:9,h:9};if(t==='bender')return{x,y,w:60,h:12};
if(t==='btnled')return{x,y,w:17,h:28};if(t==='gear')return{x,y,w:18,h:18};return{x,y,w:0,h:0}}

function drawSlider(x,y,v,er,notches){const tw=17+er;
if(notches>0){for(let i=0;i<notches;i++){const nv=i/(notches-1);const ty=Math.round(40-nv*40);ctx.fillStyle='#dbdbdb';ctx.fillRect(x+1,y+ty,tw,1)}}
else{for(let i=0;i<=10;i++){const ty=Math.round(44-i*4);ctx.fillStyle=(i===0||i===5||i===10)?'#dbdbdb':'#7e7e7e';ctx.fillRect(x+1,y+ty,tw,1)}}
ctx.fillStyle='#222';ctx.fillRect(x+7,y+1,5,47);ctx.fillRect(x+8,y,3,1);ctx.fillRect(x+8,y+48,3,1);
ctx.fillStyle='#000';ctx.fillRect(x+9,y+2,1,45);const fy=Math.round(40-v*40);
if(images.sliderHandle){const sw=images.sliderHandle.width,sh=images.sliderHandle.height;ctx.drawImage(images.sliderHandle,0,0,sw,sh,x+Math.floor((19-sw/2)/2),y+fy,sw/2,sh/2)}}

function drawSwitch(x,y,v,n){const img=n===3?images.switch3:images.switch2;if(!img)return;const fh=img.height/n;ctx.drawImage(img,0,v*fh,img.width,fh,x,y,img.width/2,fh/2)}
function drawHSwitch(x,y,v,n){const img=n===3?images.switch3h:images.switch2h;if(!img)return;const fh=img.height/n;ctx.drawImage(img,0,v*fh,img.width,fh,x,y,img.width/2,fh/2)}

function drawKnob(x,y,v){if(!images.smallknob)return;const nf=32,fw2=images.smallknob.width/nf,h2=images.smallknob.height;
const fr=Math.round(v*(nf-1)),fw=fw2/2,fh=h2/2;ctx.drawImage(images.smallknob,fr*fw2,0,fw2,h2,x+(27-fw)/2,y+(27-fh)/2,fw,fh)}

const bc=[{hi:'#fdf7cb',face:'#dcdcb2',lo:'#b2b390'},{hi:'#fdf7cb',face:'#e4e558',lo:'#b4b345'},{hi:'#fdf7cb',face:'#f0ad38',lo:'#b47e29'}];
function drawBtn(x,y,w,h,t,pr){const c=bc[t];ctx.fillStyle=c.face;ctx.fillRect(x+2,y+2,w-4,h-4);
ctx.fillStyle=pr?c.lo:c.hi;ctx.fillRect(x+1,y+1,w-2,1);ctx.fillRect(x+1,y+1,1,h-2);
ctx.fillStyle=pr?c.hi:c.lo;ctx.fillRect(x+2,y+h-2,w-3,1);ctx.fillRect(x+w-2,y+2,1,h-3);
ctx.fillStyle='#000';ctx.fillRect(x,y,w,1);ctx.fillRect(x,y+h-1,w,1);ctx.fillRect(x,y,1,h);ctx.fillRect(x+w-1,y,1,h)}

function drawPower(x,y,on){
ctx.fillStyle='#000';ctx.fillRect(x,y,15,19);
ctx.fillStyle='#808080';
ctx.fillRect(x+2,y+2,11,1);ctx.fillRect(x+2,y+16,11,1);ctx.fillRect(x+2,y+2,1,15);ctx.fillRect(x+12,y+2,1,15);
if(on){ctx.fillRect(x+4,y+11,7,1);ctx.fillStyle='#f00';ctx.fillRect(x+6,y+13,4,1)}
else{ctx.fillRect(x+4,y+5,7,1)}}

function drawClipLED(x,y){
if(!images.ledRed)return;
const fw=images.ledRed.width,fh=images.ledRed.height/2;
const srcY=scopeClipHold>0?fh:0;
ctx.drawImage(images.ledRed,0,srcY,fw,fh,x,y,fw/2,fh/2)}

function drawBender(x,y,val){
const h=12, trackY=h-8, mid=30, pi=Math.PI;
const value=val*2-1;
const angle=pi*(2-value)/4;
const trigOff=benderTriggered?-4:0;
ctx.fillStyle='#000';ctx.fillRect(x+4,y+trackY,52,8);
if(images.benderGrad){const ig=images.benderGrad;ctx.drawImage(ig,0,0,ig.width,ig.height,x+5,y+trackY+1,50,6)}
const bx1=Math.cos(angle+pi/20)*24+mid;
const bx2=Math.cos(angle-pi/20)*24+mid;
const px1=Math.cos(angle+pi/50)*36+mid;
const px2=Math.cos(angle-pi/50)*36+mid;
const baseY=y+trackY+1, pointY=y+trackY+1+trigOff;
ctx.fillStyle='#808080';ctx.beginPath();
if(bx1<px1){ctx.moveTo(x+bx1,baseY);ctx.lineTo(x+px2,pointY);ctx.lineTo(x+px2,pointY+6);ctx.lineTo(x+bx1,baseY+6)}
else{ctx.moveTo(x+px1,pointY);ctx.lineTo(x+bx2,baseY);ctx.lineTo(x+bx2,baseY+6);ctx.lineTo(x+px1,pointY+6)}
ctx.closePath();ctx.fill();
ctx.fillStyle='#fff';ctx.beginPath();
ctx.moveTo(x+px1,pointY);ctx.lineTo(x+px2+1,pointY);ctx.lineTo(x+px2+1,pointY+6);ctx.lineTo(x+px1,pointY+6);
ctx.closePath();ctx.fill();
ctx.fillStyle='#808080';
ctx.fillRect(x+px1,pointY,1,1);ctx.fillRect(x+px1,pointY+5,1,1);
ctx.fillRect(x+px2,pointY,1,1);ctx.fillRect(x+px2,pointY+5,1,1)}

function drawBtnLED(x,y,on,bt){if(images.ledRed){const fw=images.ledRed.width,fh=images.ledRed.height/2;
ctx.drawImage(images.ledRed,0,on?fh:0,fw,fh,x+4,y,fw/2,fh/2)}drawBtn(x,y+9,17,19,bt,false)}

// Keyboard
const K0='#000',K1='#808080',K2='#b3b3b3',K3='#ddd',K4='#fff';
function _h(c,a,y,b){ctx.fillStyle=c;ctx.fillRect(a,y,b-a,1)}function _v(c,x,a,b){ctx.fillStyle=c;ctx.fillRect(x,a,1,b-a)}
function _p(c,x,y){ctx.fillStyle=c;ctx.fillRect(x,y,1,1)}function _f(c,l,t,r,b){ctx.fillStyle=c;ctx.fillRect(l,t,r-l,b-t)}

function KBb(o,y,p){if(p){_p(K4,o+3,y+105);_h(K3,o+4,y+105,o+20);_h(K2,o+3,y+106,o+20);_h(K2,o+2,y+107,o+21);_p(K1,o+20,y+106);_v(K0,o+1,y+106,y+108);_v(K0,o+21,y+106,y+108);_h(K0,o+1,y+108,o+22)}else{_h(K2,o+3,y+105,o+20);_h(K2,o+2,y+106,o+21);_h(K0,o,y+107,o+22)}}
function KBl(o,y,p){_f(K3,o+4,y,o+11,y+57);_f(K3,o+4,y+57,o+20,y+105);_v(K0,o,y,y+108);_p(K0,o+1,y+106);_v(p?K0:K1,o+1,y,y+106);_v(p?K1:K4,o+2,y,y+107);_v(K4,o+3,y,y+106);KBb(o,y,p);_f(K1,o+11,y,o+13,y+57);_h(K1,o+11,y+56,o+21);_f(K1,o+20,y+56,o+22,y+106);if(p){_f(K1,o+11,y+57,o+13,y+59);_f(K0,o+13,y+56,o+22,y+59)}_p(K0,o+21,y+106)}
function KB1(o,y,p){_f(K3,o+7,y,o+16,y+57);_f(K3,o+4,y+57,o+20,y+105);_v(p?K0:K1,o+5,y,y+57);_h(K1,o+1,y+56,o+5);_v(p?K0:K1,o+1,y+56,y+107);_v(K0,o,y+56,y+107);_p(K0,o+1,y+106);_v(K4,o+6,y,y+57);_v(p?K1:K4,o+2,y+57,y+107);_v(K4,o+3,y+57,y+106);if(p){_f(K0,o+2,y+56,o+6,y+59);_v(K4,o+6,y+56,y+59)}KBb(o,y,p);_f(K1,o+16,y,o+18,y+57);_h(K1,o+18,y+56,o+20);_f(K1,o+20,y+56,o+22,y+106);_p(K0,o+21,y+106);if(p){_f(K1,o+16,y+56,o+18,y+59);_f(K0,o+18,y+56,o+22,y+59)}}
function KB2(o,y,p){_f(K3,o+7,y,o+13,y+57);_f(K3,o+4,y+57,o+20,y+105);_v(K1,o+5,y,y+57);_h(K1,o+1,y+56,o+5);_v(p?K0:K1,o+1,y+56,y+107);_v(K0,o,y+56,y+107);_p(K0,o+1,y+106);_v(K4,o+6,y,y+57);_v(p?K1:K4,o+2,y+57,y+107);_v(K4,o+3,y+57,y+106);if(p){_f(K0,o+1,y+56,o+5,y+59);_v(K1,o+5,y+56,y+59);_v(K4,o+6,y+56,y+59)}KBb(o,y,p);_p(K0,o+21,y+106);_f(K1,o+13,y,o+15,y+57);_h(K1,o+15,y+56,o+20);_f(K1,o+20,y+56,o+22,y+106);if(p){_f(K1,o+13,y+56,o+15,y+59);_f(K0,o+15,y+56,o+22,y+59)}}
function KB3(o,y,p){_f(K3,o+9,y,o+16,y+57);_f(K3,o+4,y+57,o+20,y+105);_v(K1,o+7,y,y+57);_h(K1,o+1,y+56,o+7);_v(p?K0:K1,o+1,y+56,y+107);_v(K0,o,y+56,y+107);_p(K0,o+1,y+106);_v(K4,o+8,y,y+57);_v(p?K1:K4,o+2,y+57,y+107);_v(K4,o+3,y+57,y+106);if(p){_f(K0,o+1,y+56,o+7,y+59);_v(K1,o+7,y+56,y+59);_v(K4,o+8,y+56,y+59)}KBb(o,y,p);_p(K0,o+21,y+106);_f(K1,o+16,y,o+18,y+57);_h(K1,o+18,y+56,o+20);_f(K1,o+20,y+56,o+22,y+106);if(p){_f(K1,o+16,y+56,o+18,y+59);_f(K0,o+18,y+56,o+22,y+59)}}
function KBr(o,y,p){_f(K3,o+12,y,o+20,y+57);_f(K3,o+4,y+57,o+20,y+105);_v(K0,o,y+56,y+108);_p(K0,o+1,y+106);_v(K1,o+10,y,y+57);_h(K1,o+1,y+56,o+10);_v(p?K0:K1,o+1,y+56,y+107);_v(K4,o+11,y,y+57);_v(p?K1:K4,o+2,y+57,y+107);_v(K4,o+3,y+57,y+106);if(p){_v(K1,o+10,y+56,y+59);_v(K4,o+11,y+56,y+59);_f(K0,o+1,y+56,o+10,y+59)}KBb(o,y,p);_f(K1,o+20,y,o+22,y+106);_p(K0,o+21,y+106)}
function KBk(o,y,p){const d=p?2:0;_f(K0,o,y,o+14,y+56);_v(K1,o+1,y,y+51);_v(K1,o+1,y+52,y+55);_v(K3,o+2,y+49+d,y+51+d);_h(K3,o+3,y+51+d,o+5)}
function KBe(o,y,p){_f(K3,o+4,y,o+20,y+107);_v(K0,o,y,y+108);_p(K0,o+1,y+106);_v(p?K0:K1,o+1,y,y+106);_v(p?K1:K4,o+2,y,y+107);_v(K4,o+3,y,y+106);if(p){_h(K2,o+3,y+106,o+20);_h(K2,o+2,y+107,o+21);_h(K0,o,y+108,o+22)}else{_h(K2,o+3,y+105,o+20);_h(K2,o+2,y+106,o+21);_h(K0,o,y+107,o+22)}_f(K1,o+20,y,o+22,y+106);_v(K0,o+22,y,y+108);_p(K0,o+21,y+106)}

const KB={x:129,y:106,w:792,h:114,minNote:36,numKeys:61};
const kbKeys=new Uint8Array(61);

function drawKeyboard(){const ox=KB.x,oy=KB.y+5;ctx.fillStyle=K3;ctx.fillRect(ox,oy,792,107);
for(let oc=0;oc<5;oc++){const x=ox+oc*154,k=oc*12;KBl(x,oy,kbKeys[k]);KBk(x+13,oy,kbKeys[k+1]);KB1(x+22,oy,kbKeys[k+2]);KBk(x+40,oy,kbKeys[k+3]);KBr(x+44,oy,kbKeys[k+4]);KBl(x+66,oy,kbKeys[k+5]);KBk(x+79,oy,kbKeys[k+6]);KB2(x+88,oy,kbKeys[k+7]);KBk(x+103,oy,kbKeys[k+8]);KB3(x+110,oy,kbKeys[k+9]);KBk(x+128,oy,kbKeys[k+10]);KBr(x+132,oy,kbKeys[k+11])}KBe(ox+770,oy,kbKeys[60]);
// Transpose chevron in 5px strip above keys
if(typeof transposeKey!=='undefined'&&transposeKey>=0){
  const xOfs=[0,13,22,40,44,66,79,88,103,110,128,132];
  const cxOfs=[7,7,12,7,16,7,7,10,7,13,7,16];
  const kx=(transposeKey===60)?ox+770:ox+Math.floor(transposeKey/12)*154+xOfs[transposeKey%12];
  const cx=kx+(transposeKey===60?12:cxOfs[transposeKey%12]);
  const cy=KB.y+2;
  ctx.fillStyle='#0f0';ctx.beginPath();ctx.moveTo(cx-3,cy);ctx.lineTo(cx+3,cy);ctx.lineTo(cx,cy+4);ctx.closePath();ctx.fill()}}

function drawPresetDisplay(){ctx.fillStyle='#000';ctx.fillRect(790,86,128,14);ctx.save();ctx.beginPath();ctx.rect(790,86,128,14);ctx.clip();ctx.fillStyle='#0dc000';ctx.font="11px 'Segment14', monospace";const name=!synth.ready?'Loading...':presetDirty?'Manual':synth.getPresetName(currentPreset);ctx.fillText(name,793,97);ctx.restore()}

// Pre-parsed SVG gear path from plugin (Tabler Icons, 24x24 viewBox)
const _gearPath=new Path2D('M10.325 4.317c.426 -1.756 2.924 -1.756 3.35 0a1.724 1.724 0 0 0 2.573 1.066c1.543 -.94 3.31 .826 2.37 2.37a1.724 1.724 0 0 0 1.065 2.572c1.756 .426 1.756 2.924 0 3.35a1.724 1.724 0 0 0 -1.066 2.573c.94 1.543 -.826 3.31 -2.37 2.37a1.724 1.724 0 0 0 -2.572 1.065c-.426 1.756 -2.924 1.756 -3.35 0a1.724 1.724 0 0 0 -2.573 -1.066c-1.543 .94 -3.31 -.826 -2.37 -2.37a1.724 1.724 0 0 0 -1.065 -2.572c-1.756 -.426 -1.756 -2.924 0 -3.35a1.724 1.724 0 0 0 1.066 -2.573c-.94 -1.543 .826 -3.31 2.37 -2.37c1 .608 2.296 .07 2.572 -1.065');
const _gearCircle=new Path2D('M9 12a3 3 0 1 0 6 0a3 3 0 0 0 -6 0');

function drawGear(x,y,hover){
  const s=18/24;
  ctx.save();
  ctx.translate(x,y);
  ctx.scale(s,s);
  ctx.strokeStyle=hover?'#00ff00':'#808080';
  ctx.lineWidth=1.5/s;
  ctx.lineCap='round';ctx.lineJoin='round';
  ctx.stroke(_gearPath);
  ctx.stroke(_gearCircle);
  ctx.restore()}

// Settings menu state (shared with kr106-ui.js)
let menuOpen=false,menuHover=-1;
const menuItems=[
  {type:'radio',text:'06 Voices',group:'voices',val:6},
  {type:'radio',text:'08 Voices',group:'voices',val:8},
  {type:'radio',text:'10 Voices',group:'voices',val:10},
  {type:'sep'},
  {type:'check',text:'Ignore MIDI Velocity',key:'ignoreVel'},
  {type:'check',text:'Enable MIDI Input',key:'midiEnabled'},
];
let menuSettings={voices:6,ignoreVel:true,midiEnabled:false};

function drawSettingsMenu(){
  ctx.save();ctx.scale(S,S);
  const mw=180,mh=menuItems.length*14;
  const mx=Math.round((W-mw)/2),my=Math.round((H-mh)/2);
  // Background
  ctx.fillStyle='#000';ctx.fillRect(mx,my,mw,mh);
  ctx.strokeStyle='#006400';ctx.lineWidth=1;ctx.strokeRect(mx+0.5,my+0.5,mw-1,mh-1);
  // Items
  ctx.font="11px 'Segment14', monospace";
  for(let i=0;i<menuItems.length;i++){
    const it=menuItems[i];
    const iy=my+i*14;
    if(it.type==='sep'){ctx.fillStyle='#004000';ctx.fillRect(mx+1,iy+6,mw-2,1);continue}
    if(it.type==='label'){ctx.fillStyle='#008000';ctx.fillText(it.text,mx+6,iy+11);continue}
    const hov=i===menuHover;
    let active=false;
    if(it.type==='radio')active=(menuSettings[it.group]===it.val);
    else if(it.type==='check')active=!!menuSettings[it.key];
    if(hov){ctx.fillStyle='#003c00';ctx.fillRect(mx+1,iy,mw-2,14)}
    ctx.fillStyle='#00ff00';
    const prefix=active?'* ':'  ';
    ctx.fillText(prefix+it.text,mx+6,iy+11)}
  ctx.restore()}
