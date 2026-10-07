import {summarize,csv} from './metrics.js';
const $ = id => document.getElementById(id);
let device=null, view='setup', busy=false, queue=Promise.resolve(), held=false, priorDown=false;
let point={x:160,y:160}, keys=new Set(), selections=0, scrollTotal=0, dragDistance=0, dragTarget=null;
let traces=[], lastCal='IDLE', toastTimer, recording=false;
let lab=null, rows=[], sessionId=crypto.randomUUID();
const instructions={REST:['Remain comfortably still','Learning resting variation and gyro bias.'],LEFT:['Turn comfortably left','Measuring comfortable leftward control.'],RIGHT:['Turn comfortably right','Measuring comfortable rightward control.'],UP:['Look comfortably up','Measuring comfortable upward control.'],DOWN:['Look comfortably down','Measuring comfortable downward control.'],NATURAL:['Move naturally','Collecting a final sample window.'],ANALYZE:['Analyzing motion','Calculating deadzones and directional gain.'],VALIDATE:['Validating your profile','Checking all parameters against allowed bounds.'],PROFILE_SAVE:['Saving your profile','Verifying the new slot before accepting it.'],COMPLETE:['NodX adapted. Control ready.','Your simulated motion profile is saved. Resume when you are ready.'],FAILED:['Calibration did not complete','Your last valid profile is preserved.']};
async function request(data, endpoint='/api/device') {
  const run=async()=>{
    const response=await fetch(endpoint,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});
    const result=await response.json();
    if(!response.ok) throw new Error(result.error||'Device request failed');
    return result;
  };
  const task=queue.then(run);queue=task.catch(()=>{});return task;
}
async function action(action,extra={}) {
  try {
    const data=await request({action,...extra});render(data,false);
    if(!data.ok) toast('Action refused. Check profile, connection and healthy samples.');
    return data;
  } catch(error) {toast(error.message);return null;}
}
function toast(text) { $('toast').textContent=text;$('toast').classList.add('show');clearTimeout(toastTimer);toastTimer=setTimeout(()=>$('toast').classList.remove('show'),4500); }
function download(name,text,type='text/plain') {
  const url=URL.createObjectURL(new Blob([text],{type}));const a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
}
function space() {return view==='lab'?$('arena'):$('studio');}
function showView(next) {
  if(lab) abortLab('navigation');
  keys.clear();held=false;view=next;
  document.querySelectorAll('.view').forEach(v=>v.hidden=v.id!==next);
  document.querySelectorAll('.nav').forEach(b=>b.classList.toggle('active',b.dataset.view===next));
  const box=space().getBoundingClientRect();point={x:box.width/2,y:box.height/2};renderCursor();
}
function renderCursor() {
  const cursor=$('nodxCursor');
  cursor.hidden=view==='setup';
  if(cursor.hidden)return;
  const rect=space().getBoundingClientRect();cursor.style.left=(rect.left+point.x)+'px';cursor.style.top=(rect.top+point.y)+'px';
  const host=view==='lab'&&$('inputSource').value==='HOST_POINTER'&&device?.source!=='HARDWARE';
  const state=host?'NORMAL':device?.cursor||'WARNING';cursor.className='nodx-cursor '+state;$('cursorText').textContent=host?'HOST POINTER':state.replaceAll('_',' ');
  cursor.querySelector('.dwell-ring').style.strokeDashoffset=126*(1-(device?.dwellProgress||0));
}
function render(data,applyReports=true) {
  device=data;
  const hardware=data.source!=='SIMULATED';
  $('deviceSource').textContent=data.source==='FIRMWARE_SIMULATED'?'FIRMWARE SIMULATOR':hardware?'HARDWARE DEVICE':'SIMULATED DEVICE';
  $('calSource').textContent=data.source==='FIRMWARE_SIMULATED'?'SYNTHETIC SENSOR':hardware?'LIVE SENSOR':'SYNTHETIC USER C';
  $('deviceDetail').textContent=hardware?'USB telemetry · BLE HID output':'Virtual sensor · Shared firmware engine';
  $('calEvidence').textContent=hardware?'Live sensor samples. Comfortable movement only; parameters still require validation.':'Demo samples are mathematical inputs, not clinical or user evidence.';
  $('gainEvidence').textContent=hardware?'Directional gain: output pixels / degree. Generated from live samples.':'Directional gain: output pixels / degree. Generated from simulated samples.';
  for(const id of ['yaw','pitch','roll','switch','fault','ble','corrupt','record'])$(id).disabled=hardware;
  if(hardware && $('inputSource').value!=='HOST_POINTER')$('inputSource').value='HOST_POINTER';
  $('connection').innerHTML='<i></i>ENGINE CONNECTED';$('systemState').textContent=data.state;
  $('profileState').textContent=data.hasProfile?'VALID':'REQUIRED';$('faultCount').textContent=data.faults;
  $('reason').textContent=data.reason;$('sessionStatus').textContent=data.state.replaceAll('_',' ');
  $('cursorLabel').textContent=data.cursor.replaceAll('_',' ');
  $('dwellLabel').textContent=`Dwell: ${data.dwell} · ${data.cancellations} cancellations`;
  $('dwellBar').style.width=data.dwellProgress*100+'%';
  $('pause').textContent=data.state==='ACTIVE'?'Ⅱ Pause':'▷ Resume';
  const cal=data.calibration;
  if(instructions[cal]) {
    $('instruction').textContent=instructions[cal][0];$('phaseDescription').textContent=cal==='FAILED'?data.calibrationReason:instructions[cal][1];
    $('phaseLabel').textContent=cal==='COMPLETE'?'PROFILE GENERATED':'CALIBRATION / '+cal;
  }
  $('calibrate').disabled=data.state==='CALIBRATING';$('cancel').disabled=data.state!=='CALIBRATING';
  $('calProgress').style.width=data.calibrationProgress*100+'%';$('calPercent').textContent=Math.round(data.calibrationProgress*100)+'%';
  document.querySelectorAll('.direction').forEach(d=>d.classList.toggle('active',d.textContent===cal));
  $('stepCal').classList.toggle('done',data.state==='CALIBRATING'||cal==='COMPLETE');$('stepProfile').classList.toggle('done',data.hasProfile);
  if(cal==='COMPLETE' && lastCal!=='COMPLETE')toast('Calibration complete. Your simulated profile is saved.');lastCal=cal;
  const p=data.profile;
  $('profileBadge').textContent=data.hasProfile?'VALID PROFILE':'NOT CALIBRATED';
  for(const [i,id] of ['gainLeft','gainRight','gainUp','gainDown'].entries())$(id).textContent=data.hasProfile?p.gain[i].toFixed(1):'—';
  $('deadzone').textContent=data.hasProfile?p.deadzone.map(x=>x.toFixed(2)).join(' / ')+' °/s':'—';
  $('alpha').textContent=data.hasProfile?p.alpha.toFixed(2):'—';
  $('dwell').checked=p.dwellEnabled;$('scroll').checked=p.scrollEnabled;
  $('stabilityValue').textContent=Math.round(data.stability*100)+'%';$('stabilityBar').style.width=data.stability*100+'%';
  $('motionValues').textContent=data.motion.slice(0,2).map(x=>x.toFixed(1)).join(' / ')+' °/s';
  $('headGraphic').style.transform=`rotate(${Math.max(-25,Math.min(25,data.motion[2]))}deg) translate(${data.motion[0]*.3}px,${data.motion[1]*.3}px)`;
  traces.push(data.motion.slice(0,2));if(traces.length>100)traces.shift();drawTrace();
  if(applyReports && (view==='control'||(view==='lab'&&$('inputSource').value==='SIMULATED'))) {
    const bounds=space().getBoundingClientRect();
    for(const [dx,dy,wheel,down] of data.reports) {
      const old={...point};point.x=Math.max(0,Math.min(bounds.width-1,point.x+dx));point.y=Math.max(0,Math.min(bounds.height-1,point.y+dy));scrollTotal+=wheel;
      if(down && !priorDown && view==='control') {
        dragTarget=[...document.querySelectorAll('.practice-target')].find(t=>insideElement(t,point))||null;
      }
      if(down && dragTarget) {
        dragTarget.style.left=(dragTarget.offsetLeft+point.x-old.x)+'px';dragTarget.style.top=(dragTarget.offsetTop+point.y-old.y)+'px';
        dragDistance+=Math.hypot(point.x-old.x,point.y-old.y);
      }
      if(!down && priorDown && data.state==='ACTIVE')selectAt(point.x,point.y);
      if(!down)dragTarget=null;
      priorDown=!!down;
    }
    $('studioResult').textContent=`Selections: ${selections} · Scroll: ${scrollTotal} · Drag distance: ${dragDistance.toFixed(0)} px`;
  }
  if(lab && data.state!=='ACTIVE' && lab.source==='SIMULATED')abortLab('device stopped');
  renderCursor();
}
function insideElement(element,p) {return p.x>=element.offsetLeft && p.x<=element.offsetLeft+element.offsetWidth && p.y>=element.offsetTop && p.y<=element.offsetTop+element.offsetHeight;}
function drawTrace() {
  const c=$('trace'),ctx=c.getContext('2d');const width=c.clientWidth||320;const ratio=devicePixelRatio||1;
  if(c.width!==Math.round(width*ratio)){c.width=Math.round(width*ratio);c.height=105*ratio;}
  ctx.setTransform(ratio,0,0,ratio,0,0);ctx.clearRect(0,0,width,105);
  ctx.strokeStyle='#24363c';ctx.lineWidth=1;
  for(let y=15;y<105;y+=25){ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(width,y);ctx.stroke();}
  ['#64ead7','#b0bdff'].forEach((color,axis)=>{
    ctx.strokeStyle=color;ctx.lineWidth=1.5;ctx.beginPath();traces.forEach((p,i)=>{const x=i*width/99,y=52-Math.max(-40,Math.min(40,p[axis]));if(i)ctx.lineTo(x,y);else ctx.moveTo(x,y);});ctx.stroke();
  });
}
function selectAt(x,y) {
  if(view==='control'){if([...document.querySelectorAll('.practice-target')].some(t=>insideElement(t,{x,y})))selections++;return;}
  if(view!=='lab'||!lab)return;
  const hit=Math.hypot(x-lab.target.x,y-lab.target.y)<=lab.target.width/2;
  if(lab.waitingStart){if(hit){lab.waitingStart=false;lab.last={x,y};nextTarget();}else toast('Select the center starting target first.');return;}
  logAttempt(x,y,hit,false);
  lab.last={x,y};lab.index++;
  if(lab.index>=12)finishLab();else nextTarget();
}
function placeTarget(target) {
  const el=$('labTarget');el.hidden=false;el.style.width=target.width+'px';el.style.height=target.width+'px';el.style.left=(target.x-target.width/2)+'px';el.style.top=(target.y-target.width/2)+'px';
}
function nextTarget() {
  const bounds=$('arena').getBoundingClientRect();const i=lab.index;
  const angle=i*Math.PI*.75;const radius=Math.min(bounds.width*.32,bounds.height*.32)*(i%2?.85:1);
  lab.target={x:bounds.width/2+Math.cos(angle)*radius,y:bounds.height/2+Math.sin(angle)*radius,width:[32,56,80][i%3]};
  lab.distance=Math.hypot(lab.target.x-lab.last.x,lab.target.y-lab.last.y);
  lab.started=performance.now();lab.startWall=new Date().toISOString();lab.startDevice=device?.timeMs||0;lab.cancelStart=device?.cancellations||0;
  placeTarget(lab.target);$('trialLabel').textContent=`TRIAL ${i+1} / 12 · ${lab.condition} · ${lab.source}`;
}
function logAttempt(x,y,hit,aborted,reason='') {
  const time=Math.max(.001,performance.now()-lab.started);
  const row={sessionId,blockId:lab.id,trial:lab.index+1,condition:lab.condition,inputSource:lab.source,deviceSource:device?.source||'UNKNOWN',selectionMethod:lab.profile.dwellEnabled?'SWITCH_OR_DWELL':'SWITCH',
    targetX:lab.target.x,targetY:lab.target.y,width:lab.target.width,startX:lab.last.x,startY:lab.last.y,endX:x,endY:y,distance:lab.distance,
    startTime:lab.startWall,selectionTime:new Date().toISOString(),movementTimeMs:time,nominalId:Math.log2(lab.distance/lab.target.width+1),hit,aborted,abortReason:reason,
    deviceStartMs:lab.startDevice,deviceEndMs:device?.timeMs||0,dwellCancellations:(device?.cancellations||0)-lab.cancelStart,profile:structuredClone(lab.profile),
    viewportWidth:$('arena').clientWidth,viewportHeight:$('arena').clientHeight,devicePixelRatio,softwareVersion:'0.1.0'};
  rows.push(row);request(row,'/api/trial').then(r=>{row.profileHash=r.profileHash;row.persisted=true;}).catch(e=>{row.persisted=false;toast('Trial is in CSV memory; disk logging failed: '+e.message);});renderMetrics();
}
async function startLab() {
  if(lab)return;
  const condition=$('condition').value,source=$('inputSource').value;
  let result=await action(condition==='GENERIC'?'generic':'load');
  if(!result?.ok){toast('Calibrate and save an adaptive profile first.');return;}
  // Never silently change selection settings during a condition.
  if(source==='SIMULATED'){result=await action('resume');if(!result?.ok)return;}
  lab={id:crypto.randomUUID(),index:0,waitingStart:true,condition,source,profile:structuredClone(result.profile)};
  const box=$('arena').getBoundingClientRect();lab.target={x:box.width/2,y:box.height/2,width:48};
  point={x:box.width/2,y:box.height/2};placeTarget(lab.target);$('labWelcome').hidden=true;
  $('trialLabel').textContent='SELECT THE CENTER TARGET TO BEGIN';$('labStatus').textContent=source;
  $('startLab').disabled=true;$('stopLab').disabled=false;
  for(const id of ['condition','inputSource','calibrate','load','dwell','scroll'])$(id).disabled=true;
  $('arena').classList.toggle('host',source==='HOST_POINTER');renderCursor();
}
function finishLab() {lab=null;$('labTarget').hidden=true;$('labWelcome').hidden=false;$('startLab').disabled=false;$('stopLab').disabled=true;for(const id of ['condition','inputSource','calibrate','load','dwell','scroll'])$(id).disabled=false;$('labStatus').textContent='BLOCK COMPLETE';renderMetrics();}
function abortLab(reason='user abort') {if(!lab)return;if(!lab.waitingStart)logAttempt(point.x,point.y,false,true,reason);finishLab();$('labStatus').textContent='ABORTED';}
function renderMetrics() {
  const node=$('metrics');node.replaceChildren();
  for(const condition of ['GENERIC','ADAPTIVE']) {
    const subset=rows.filter(r=>r.condition===condition&&r.inputSource===$('inputSource').value),m=summarize(subset);
    const card=document.createElement('div');card.className='metric-card';
    const header=document.createElement('h3');header.textContent=condition+' / '+$('inputSource').value;card.append(header);
    const fields=[['Attempts',m.attempts],['Hit rate',m.accuracy===null?'—':(100*m.accuracy).toFixed(1)+'%'],['Mean selection time',m.meanTimeMs===null?'—':(m.meanTimeMs/1000).toFixed(3)+' s'],['Misses',m.errors],['Nominal successful ID / time',m.nominalRate===null?'—':m.nominalRate.toFixed(2)+' bits/s']];
    for(const [label,value] of fields){const row=document.createElement('div');row.className='metric-row';const l=document.createElement('span'),v=document.createElement('b');l.textContent=label;v.textContent=value;row.append(l,v);card.append(row);}node.append(card);
  }
}
document.querySelectorAll('.nav').forEach(b=>b.onclick=()=>showView(b.dataset.view));
$('calibrate').onclick=()=>action('calibrate');$('cancel').onclick=()=>action('cancel');$('resume').onclick=()=>action('resume');
$('pause').onclick=()=>action(device?.state==='ACTIVE'?'pause':'resume');$('load').onclick=()=>action('load');
$('exportProfile').onclick=()=>{if(!device?.hasProfile){toast('Create a valid profile first.');return;}download('nodx-profile.json',JSON.stringify({source:device.source,softwareVersion:'0.1.0',parameters:device.profile},null,2),'application/json');};
$('dwell').onchange=()=>action('dwell',{enabled:$('dwell').checked});$('scroll').onchange=()=>action('scroll',{enabled:$('scroll').checked});
$('corrupt').onclick=()=>action('corrupt');
$('record').onclick=async()=>{const r=await action('record',{enabled:!recording});if(r?.ok){recording=!recording;$('record').textContent=recording?'Stop raw recording':'Record raw samples';toast(recording?'Recording every native sample to runtime/samples.csv.':'Raw recording saved in runtime/samples.csv.');}};
for(const axis of ['yaw','pitch','roll'])$(axis).oninput=()=>$(axis+'Out').textContent=$(axis).value+' °/s';
const setHeld=value=>{held=value;$('switch').classList.toggle('held',value);};
$('switch').onpointerdown=e=>{e.preventDefault();$('switch').setPointerCapture(e.pointerId);setHeld(true);};
$('switch').onpointerup=()=>setHeld(false);$('switch').onpointercancel=()=>setHeld(false);
$('reduced').checked=matchMedia('(prefers-reduced-motion:reduce)').matches;
$('reduced').onchange=()=>document.body.classList.toggle('reduced',$('reduced').checked);$('reduced').onchange();
$('glow').onchange=()=>document.body.classList.toggle('no-glow',!$('glow').checked);$('scale').onchange=()=>{document.documentElement.style.setProperty('--scale',$('scale').value);if(lab)abortLab('scale changed');};
$('startLab').onclick=startLab;$('stopLab').onclick=()=>abortLab();$('inputSource').onchange=()=>{renderMetrics();renderCursor();};
$('exportTrials').onclick=()=>{if(!rows.length){toast('No raw trials yet.');return;}download('nodx-trials.csv',csv(rows),'text/csv');};
$('arena').onclick=e=>{if(lab?.source==='HOST_POINTER'){const box=$('arena').getBoundingClientRect();selectAt(e.clientX-box.left,e.clientY-box.top);}};
for(const element of [$('studio'),$('arena')])element.addEventListener('pointermove',e=>{if(device?.source==='HARDWARE'||(view==='lab'&&$('inputSource').value==='HOST_POINTER')){const box=element.getBoundingClientRect();point={x:e.clientX-box.left,y:e.clientY-box.top};renderCursor();}});
window.addEventListener('keydown',e=>{
  if(['INPUT','SELECT','TEXTAREA'].includes(e.target.tagName))return;
  const key=e.key.toLowerCase();
  if(view!=='setup'&&['w','a','s','d','q','e',' '].includes(key)){e.preventDefault();keys.add(key);}
  if(key==='p'&&!e.repeat)action(device?.state==='ACTIVE'?'pause':'resume');
});
window.addEventListener('keyup',e=>keys.delete(e.key.toLowerCase()));
window.addEventListener('blur',()=>{keys.clear();setHeld(false);action('pause');if(lab)abortLab('window lost focus');});
document.addEventListener('visibilitychange',()=>{if(document.hidden){keys.clear();setHeld(false);action('pause');if(lab)abortLab('page hidden');}});
window.addEventListener('resize',()=>{if(lab)abortLab('viewport changed');renderCursor();});window.addEventListener('scroll',renderCursor);
async function tick() {
  if(busy||document.hidden)return;busy=true;
  try{
    const yaw=Number($('yaw').value)+(keys.has('d')?20:0)-(keys.has('a')?20:0);
    const pitch=Number($('pitch').value)+(keys.has('s')?20:0)-(keys.has('w')?20:0);
    const roll=Number($('roll').value)+(keys.has('e')?30:0)-(keys.has('q')?30:0);
    const result=await request({action:'step',count:5,yaw,pitch,roll,pressed:held||keys.has(' '),connected:$('ble').checked,automatic:true,fault:Number($('fault').value)});
    render(result);
  }catch(error){$('connection').textContent='ENGINE OFFLINE';$('reason').textContent=error.message;if(lab)abortLab('engine offline');}
  finally{busy=false;}
}
renderMetrics();await action('status');setInterval(tick,50);tick();
