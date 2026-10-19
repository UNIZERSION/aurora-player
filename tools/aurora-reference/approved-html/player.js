/* Demo presentation layer. All timers and animation are synthetic. */
(()=>{
'use strict';
const M=window.Tab5Model,$=id=>document.getElementById(id);
const icons={
 settings:'<path d="m9.5 3-.6 2.4-1.6.9L5 5.6 2.5 9.9l1.8 1.7v1.8l-1.8 1.7L5 19.4l2.3-.7 1.6.9.6 2.4h5l.6-2.4 1.6-.9 2.3.7 2.5-4.3-1.8-1.7v-1.8l1.8-1.7L19 5.6l-2.3.7-1.6-.9-.6-2.4Z"/><circle cx="12" cy="12.5" r="3.3"/>',
 charging:'<path d="m14 5-8 9h6l-2 5 8-9h-6l2-5Z"/>',
 play:'<path d="m8 5 12 7-12 7Z"/>',pause:'<rect x="6" y="4" width="4" height="16" rx="1"/><rect x="15" y="4" width="4" height="16" rx="1"/>',
 previous:'<path d="M5 5v14M19 5 7 12l12 7Z"/>',next:'<path d="M19 5v14M5 5l12 7-12 7Z"/>',
 heart:'<path d="M20.6 4.7a5.4 5.4 0 0 0-7.6 0L12 5.8l-1.1-1.1a5.4 5.4 0 0 0-7.6 7.6L12 21l8.6-8.7a5.4 5.4 0 0 0 0-7.6Z"/>',
 shuffle:'<path d="M3 6h3c5 0 7 12 12 12h3M17 14l4 4-4 4M3 18h3c2 0 4-3 5-6s4-6 7-6h3M17 2l4 4-4 4"/>',repeat:'<path d="m17 2 4 4-4 4M3 11V9a3 3 0 0 1 3-3h15M7 22l-4-4 4-4m14-1v2a3 3 0 0 1-3 3H3"/>',
 bluetooth:'<path d="m7 7 10 10-5 5V2l5 5L7 17"/>',battery:'<rect x="2" y="6" width="18" height="12" rx="2"/><path d="M23 10v4"/><path d="M5 9h11v6H5Z" fill="currentColor" stroke="none"/>',
 volume:'<path d="M3 9h4l5-4v14l-5-4H3ZM16 8a6 6 0 0 1 0 8m3-11a10 10 0 0 1 0 14"/>',muted:'<path d="M3 9h4l5-4v14l-5-4H3Zm13 0 6 6m0-6-6 6"/>',
 bars:'<path d="M4 15v5M9 9v11m6-17v17m5-10v10" stroke-width="3"/>',bass:'<path d="M4 4v16m5-13v13m6-8v8m5-5v5" stroke-width="3"/>',orbit:'<ellipse cx="12" cy="12" rx="11" ry="4" transform="rotate(-30 12 12)"/><ellipse cx="12" cy="12" rx="6" ry="10" transform="rotate(25 12 12)"/>',tape:'<rect x="2" y="5" width="20" height="15" rx="2"/><circle cx="7" cy="11" r="2"/><circle cx="17" cy="11" r="2"/><path d="m6 20 2-4h8l2 4M10 11h4"/>',wave:'<path d="M2 13c4-14 6-14 10 0s6 14 10 0"/>',dots:'<path d="M5 5h.01M12 5h.01M19 5h.01M5 12h.01M12 12h.01M19 12h.01M5 19h.01M12 19h.01M19 19h.01" stroke-width="3"/>',disc:'<circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="2"/><path d="M12 6a6 6 0 0 1 6 6"/>',close:'<path d="m6 6 12 12M6 18 18 6"/>',check:'<path d="m5 12 4 4L20 5"/>',trash:'<path d="M3 6h18M5 6l1 15h12l1-15M9 6V3h6v3M10 10v7m4-7v7"/>'};
const icon=name=>`<svg class="icon ${name}" viewBox="0 0 24 24" aria-hidden="true">${icons[name]||icons.disc}</svg>`;
document.querySelectorAll('[data-icon]').forEach(e=>e.innerHTML=icon(e.dataset.icon));
const percent=n=>`${n}<span class="percent-sign">%</span>`;
const time=n=>`${Math.floor(n/60).toString().padStart(2,'0')}:${Math.floor(n%60).toString().padStart(2,'0')}`;
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const artwork=t=>`<svg viewBox="${t.art.join(' ')}" preserveAspectRatio="xMidYMid slice" aria-hidden="true"><image href="assets/reference.png" width="1696" height="928"/></svg>`;
let state=M.initialState(),scrubbing=false,toastTimer,btTimer,animationPhase=0,dragonPhase=0,dragonRenderer=null,lastFrame=performance.now(),lastPaint=0;
const reducedMotion=window.matchMedia('(prefers-reduced-motion: reduce)');
const repeatLabels={off:'off',all:'all tracks',one:'one track'};
const fxLabels={normal:'Normal',bass:'Bass+',space:'Space',retro:'Retro'};
const vizLabels={spectrum:'SPECTRUM',wave:'WAVE',particles:'PARTICLES'};
const btLabels={off:'Off',scanning:'Searching for devices…',ready:'Ready to connect',connecting:'Connecting…',connected:'Connected · demo',error:'Connection failed'};
function toast(message){$('toast').textContent=message;$('toast').classList.add('visible');clearTimeout(toastTimer);toastTimer=setTimeout(()=>$('toast').classList.remove('visible'),2600);}
function dispatch(type,payload){
 const previous=state;state=M.reduce(state,{type,payload});
 if(type==='TICK'&&state.trackId===previous.trackId&&state.playing===previous.playing){renderProgress();return;}
 render(type);
 if(type!=='TICK')document.dispatchEvent(new CustomEvent('tab5:action',{detail:{type,payload:payload??null,state:JSON.parse(JSON.stringify(state)),demo:true}}));
}
function renderProgress(){const t=M.track(state);if(!scrubbing)$('seek').value=state.position;$('seek').max=t.duration;$('seek').style.setProperty('--fill',`${state.position/t.duration*100}%`);$('seek').setAttribute('aria-valuetext',`${time(state.position)} of ${time(t.duration)}`);$('elapsed').textContent=time(state.position);$('duration').textContent=time(t.duration);}
function render(type){
 const t=M.track(state);
 $('battery-level').innerHTML=percent(state.battery.level);$('charging-indicator').hidden=!state.battery.charging;$('battery-status').setAttribute('aria-label',`Demo battery: ${state.battery.level} percent, ${state.battery.charging?'charging':'on battery'}`);
 $('song-title').textContent=t.title;$('song-title').title=t.title;$('artist').textContent=t.artist;$('album').textContent=t.album;
 if($('cover').dataset.track!==t.id){$('cover').innerHTML=artwork(t);$('cover').dataset.track=t.id;$('cover').setAttribute('aria-label',`Album cover: ${t.album}`);$('cover').setAttribute('role','img');}
 $('track-counter').textContent=`${(state.queue.indexOf(t.id)+1).toString().padStart(2,'0')} / ${state.queue.length.toString().padStart(2,'0')}`;
 $('file-format').textContent=t.format;$('file-quality').innerHTML=`<span class="quality-line">${t.sampleRate/1000} <small>kHz</small></span><span class="quality-line">${t.bitrate} <small>kbps</small></span>${t.bitDepth?`<span class="bit-depth">${t.bitDepth} bit · PCM</span>`:''}`;$('quality-mini').textContent=`${t.format} · ${t.bitrate} kbps`;
 $('play-state').textContent=state.playing?'NOW PLAYING':state.position>=t.duration?'END':'PAUSED';$('play-led').classList.toggle('active',state.playing);
 $('play').innerHTML=icon(state.playing?'pause':'play');$('play').classList.toggle('playing',state.playing);$('play').setAttribute('aria-label',state.playing?'Pause demo':'Play demo');
 $('favorite').setAttribute('aria-pressed',state.favorites.includes(t.id));$('favorite').setAttribute('aria-label',state.favorites.includes(t.id)?'Remove from favorites':'Add to favorites');
 $('shuffle').setAttribute('aria-pressed',state.shuffle);$('repeat').setAttribute('aria-pressed',state.repeat!=='off');$('repeat').setAttribute('aria-label',`Repeat: ${repeatLabels[state.repeat]}`);$('repeat-one').hidden=state.repeat!=='one';
 $('next').disabled=state.repeat==='off'&&(state.shuffle?!state.shuffleBag.length:state.trackId===state.queue.at(-1));
 $('volume').value=state.volume;$('volume').style.setProperty('--fill',`${state.muted?0:state.volume}%`);$('volume').setAttribute('aria-valuetext',`${state.volume} percent${state.muted?', muted':''}`);$('volume-value').innerHTML=state.muted?'MUTE':percent(state.volume);$('volume-knob').style.setProperty('--rotation',`${-135+state.volume*2.7}deg`);$('volume-knob').setAttribute('aria-valuenow',state.volume);$('volume-knob').setAttribute('aria-valuetext',`${state.volume} percent${state.muted?', muted':''}`);
 $('mute').setAttribute('aria-pressed',state.muted);$('mute').innerHTML=icon(state.muted||state.volume===0?'muted':'volume');$('mute').setAttribute('aria-label',state.muted?'Unmute':'Mute');
 document.querySelectorAll('[data-tab]').forEach(b=>{b.setAttribute('aria-selected',b.dataset.tab===state.tab);b.tabIndex=b.dataset.tab===state.tab?0:-1;});
 ['fx','filter','viz'].forEach(key=>document.querySelectorAll(`[data-${key}]`).forEach(b=>b.setAttribute('aria-pressed',b.dataset[key]===state[key==='viz'?'visualization':key])));
 $('viz-caption').textContent=`${vizLabels[state.visualization]} / ${state.playing?'DEMO': 'PAUSED'}${state.fx==='normal'?'':' / '+fxLabels[state.fx].toUpperCase()}`;
 $('visualizer').setAttribute('aria-label',`Demo visualization: ${vizLabels[state.visualization]}, ${state.playing?'active':'paused'}`);
 renderProgress();
 if(!['VOLUME','SEEK','MUTE','FX','VISUALIZATION','BRIGHTNESS','CLOCK_FORMAT','DRAGON_TYPE','DRAGON_SPEED','DRAGON_COLORS'].includes(type))renderList();
 renderSettings();renderBluetooth();drawVisualizer();drawDragon();
}
function renderList(){
 const target=$('track-list'),focus=document.activeElement,focusKey=target.contains(focus)?focus.dataset.key:null,scroll=target.scrollTop;
 const settingsOpen=state.tab==='settings';target.hidden=settingsOpen;$('settings-panel').hidden=!settingsOpen;
 $('list-label').textContent=settingsOpen?'SETTINGS':state.tab==='favorites'?'YOUR FAVORITES':state.tab==='local'?'FILES ON SD CARD':'NEON COLLECTION';
 if(settingsOpen){$('list-count').textContent='';$('queue-status').textContent='DRAGON · DISPLAY · CLOCK';$('queue-duration').textContent='SCROLL ↕';return;}
 target.setAttribute('aria-label',{queue:'Queue',local:'Files',favorites:'Favorites'}[state.tab]);
 const rows=M.visibleTracks(state);
 target.innerHTML=rows.length?rows.map(t=>`<div class="track-row${t.id===state.trackId?' current':''}${state.playing?' is-playing':''}"><button class="track-button" data-track="${t.id}" data-key="${t.id}" aria-label="Play ${esc(t.title)}, ${t.format}"${t.id===state.trackId?' aria-current="true"':''}><span class="thumb">${artwork(t)}</span><span class="track-text"><strong>${esc(t.title)}</strong><small>${esc(t.artist)} · ${t.format}</small></span><span class="track-time">${time(t.duration)}</span>${t.id===state.trackId?'<span class="mini-eq" aria-hidden="true"><i></i><i></i><i></i></span>':''}</button></div>`).join(''):`<div class="empty">${icon('heart')}<strong>No tracks</strong>${state.tab==='favorites'?'Tap the heart beside a title to add a favorite.':'Try another file filter.'}${state.filter!=='all'?'<br>Active filter: '+state.filter:''}</div>`;
 $('list-count').textContent=`${rows.length} / ${M.tracks.length} TRACKS`;
 $('queue-status').textContent=state.filter==='all'?'SD CARD · DEMO':`FILTER: ${state.filter} · DEMO`;$('queue-duration').textContent=time(rows.reduce((n,t)=>n+t.duration,0));
 target.scrollTop=scroll;if(focusKey){const next=Array.from(target.querySelectorAll('[data-key]')).find(e=>e.dataset.key===focusKey);if(next)next.focus({preventScroll:true});else target.focus({preventScroll:true});}
}
function renderSettings(){
 const s=state.settings;
 document.querySelectorAll('[data-dragon-type]').forEach(b=>b.setAttribute('aria-pressed',b.dataset.dragonType===s.dragon.type));
 $('dragon-speed').value=s.dragon.speed;$('dragon-speed').style.setProperty('--fill',((s.dragon.speed-.4)/1.2*100)+'%');
 const speed=s.dragon.speed.toLocaleString('en-GB')+'×';$('dragon-speed-value').textContent=speed;$('dragon-speed').setAttribute('aria-valuetext',speed);
 $('dragon-colors').setAttribute('aria-checked',s.dragon.colors);$('dragon-colors').textContent=s.dragon.colors?'ON':'OFF';
 $('brightness').value=s.brightness;$('brightness').style.setProperty('--fill',((s.brightness-20)/80*100)+'%');$('brightness').setAttribute('aria-valuetext',s.brightness+' percent');$('brightness-value').innerHTML=percent(s.brightness);
 $('screen-dimmer').style.opacity=(1-s.brightness/100)*.65;
 $('animations-toggle').setAttribute('aria-checked',s.animations);$('animations-toggle').textContent=s.animations?'ON':'OFF';
 $('device').dataset.animations=s.animations?'on':'off';
 $('clock-format').setAttribute('aria-pressed',s.clock24);$('clock-format').textContent=s.clock24?'24 h':'12 h';clock();
}
function renderBluetooth(){
 const bt=state.bt;$('bluetooth').classList.toggle('active',bt.status!=='off');$('bluetooth').setAttribute('aria-label',`Bluetooth — ${btLabels[bt.status]}`);$('bt-label').textContent={off:'BT OFF',scanning:'SCANNING',ready:'BT ON',connecting:'LINKING',connected:'BT DEMO',error:'BT ERROR'}[bt.status];
 $('bt-status').textContent=btLabels[bt.status];$('bt-dot').classList.toggle('on',bt.status!=='off');$('bt-dot').classList.toggle('waiting',['scanning','connecting'].includes(bt.status));$('bt-power').textContent=bt.status==='off'?'Turn on':'Turn off';
 const box=$('bt-devices');if(box.dataset.state===`${bt.status}:${bt.device}`)return;const restoreFocus=box.contains(document.activeElement);box.dataset.state=`${bt.status}:${bt.device}`;
 if(bt.status==='off')box.innerHTML='<div class="bt-message">Turn on Bluetooth to show example devices.</div>';
 else if(bt.status==='scanning')box.innerHTML='<div class="bt-message waiting">Searching… <br>Demo scan in progress.</div>';
 else if(bt.status==='connecting')box.innerHTML=`<div class="bt-message waiting">Connecting to ${esc(bt.device)}…</div>`;
 else if(bt.status==='connected')box.innerHTML=`<button class="bt-device" data-disconnect>${icon('check')}<span>${esc(bt.device)}<small>Simulated connection · no audio streaming</small></span><span>Disconnect</span></button>`;
 else if(bt.status==='error')box.innerHTML=`<div class="bt-message">${esc(bt.device)} is not responding.<br>Example connection error.</div><button id="bt-retry" data-rescan>Scan again</button>`;
 else box.innerHTML=['Studio Monitor','Pocket Speaker'].map((name,i)=>`<button class="bt-device" data-connect="${name}">${icon('bluetooth')}<span>${name}<small>${i?'Connection failure example':'Successful connection example'} · demo</small></span><span>Connect</span></button>`).join('');
 if(restoreFocus)$('bt-power').focus();
}
const click=(id,fn)=>$(id).addEventListener('click',fn);
click('play',()=>dispatch('PLAY_TOGGLE'));click('previous',()=>dispatch('PREVIOUS'));click('next',()=>dispatch('NEXT'));
click('shuffle',()=>{dispatch('SHUFFLE');toast(`Shuffle: ${state.shuffle?'on':'off'}`);});
click('repeat',()=>{dispatch('REPEAT');toast(`Repeat: ${repeatLabels[state.repeat]}`);});
click('favorite',()=>{dispatch('FAVORITE');toast(state.favorites.includes(state.trackId)?'Added to favorites':'Removed from favorites');});
click('mute',()=>dispatch('MUTE'));
$('brightness').addEventListener('input',e=>dispatch('BRIGHTNESS',e.target.value));
click('animations-toggle',()=>dispatch('ANIMATIONS'));
click('clock-format',()=>dispatch('CLOCK_FORMAT'));
click('settings-reset',()=>{dispatch('SETTINGS_RESET');toast('Dragon and display settings reset');});
$('dragon-speed').addEventListener('input',e=>dispatch('DRAGON_SPEED',e.target.value));
click('dragon-colors',()=>dispatch('DRAGON_COLORS',!state.settings.dragon.colors));
const seek=$('seek');seek.addEventListener('pointerdown',()=>{scrubbing=true;});seek.addEventListener('input',()=>dispatch('SEEK',seek.value));seek.addEventListener('change',()=>{scrubbing=false;dispatch('SEEK',seek.value);});window.addEventListener('pointerup',()=>{scrubbing=false;});window.addEventListener('pointercancel',()=>{scrubbing=false;});
window.addEventListener('blur',()=>{scrubbing=false;});$('volume').addEventListener('input',e=>dispatch('VOLUME',e.target.value));
let knobDrag=null;const knob=$('volume-knob');
knob.addEventListener('pointerdown',e=>{e.preventDefault();knob.focus();knob.setPointerCapture(e.pointerId);knobDrag={x:e.clientX,y:e.clientY,value:state.volume,moved:false};});
knob.addEventListener('pointermove',e=>{if(!knobDrag)return;const scale=knob.getBoundingClientRect().height/84,dy=(knobDrag.y-e.clientY)/scale;if(Math.abs(dy)>3||Math.abs(e.clientX-knobDrag.x)>3)knobDrag.moved=true;if(knobDrag.moved)dispatch('VOLUME',Math.round(knobDrag.value+dy));});
knob.addEventListener('pointerup',e=>{if(!knobDrag)return;if(!knobDrag.moved){const r=knob.getBoundingClientRect(),angle=Math.atan2(e.clientX-r.left-r.width/2,-(e.clientY-r.top-r.height/2))*180/Math.PI;dispatch('VOLUME',Math.round(M.clamp((angle+135)/270*100,0,100)));}knobDrag=null;});
knob.addEventListener('pointercancel',()=>{knobDrag=null;});knob.addEventListener('lostpointercapture',()=>{knobDrag=null;});
knob.addEventListener('keydown',e=>{const delta={ArrowUp:1,ArrowRight:1,ArrowDown:-1,ArrowLeft:-1,PageUp:5,PageDown:-5}[e.key];if(delta!==undefined){e.preventDefault();dispatch('VOLUME',state.volume+delta);}else if(e.key==='Home'||e.key==='End'){e.preventDefault();dispatch('VOLUME',e.key==='Home'?0:100);}});
document.querySelector('.tabs').addEventListener('keydown',e=>{const tabs=Array.from(document.querySelectorAll('[data-tab]')),i=tabs.indexOf(e.target);if(i<0)return;let n;if(e.key==='ArrowRight')n=(i+1)%tabs.length;else if(e.key==='ArrowLeft')n=(i+tabs.length-1)%tabs.length;else if(e.key==='Home')n=0;else if(e.key==='End')n=tabs.length-1;else return;e.preventDefault();dispatch('TAB',tabs[n].dataset.tab);tabs[n].focus();});
document.addEventListener('click',e=>{
 const b=e.target.closest('button');if(!b)return;
 if(b.dataset.dragonType)dispatch('DRAGON_TYPE',b.dataset.dragonType);
 if(b.dataset.track)dispatch('SELECT_TRACK',b.dataset.track);
 if(b.dataset.tab){dispatch('TAB',b.dataset.tab);$('track-list').scrollTop=0;}
 if(b.dataset.fx){dispatch('FX',b.dataset.fx);toast(`FX: ${fxLabels[state.fx]} · visual demo`);}
 if(b.dataset.filter){dispatch('FILTER',b.dataset.filter);$('track-list').scrollTop=0;toast(`Playlist filter: ${state.filter==='all'?'all files':state.filter}`);}
 if(b.dataset.viz)dispatch('VISUALIZATION',b.dataset.viz);
 if(b.dataset.connect){clearTimeout(btTimer);dispatch('BT_CONNECT',b.dataset.connect);btTimer=setTimeout(()=>dispatch('BT_RESULT',b.dataset.connect==='Studio Monitor'),1100);}
 if(b.hasAttribute('data-disconnect')){dispatch('BT_DISCONNECT');toast('Demo device disconnected');}
 if(b.hasAttribute('data-rescan')){dispatch('BT_RESCAN');scan();}
});
function scan(){clearTimeout(btTimer);btTimer=setTimeout(()=>dispatch('BT_FOUND'),1000);}
click('bluetooth',()=>{$('bt-dialog').showModal();$('bt-close').focus();});click('bt-close',()=>{$('bt-dialog').close();$('bluetooth').focus();});
click('bt-power',()=>{clearTimeout(btTimer);dispatch('BT_POWER');if(state.bt.status==='scanning')scan();});
$('bt-dialog').addEventListener('click',e=>{if(e.target===$('bt-dialog')){const r=e.target.getBoundingClientRect();if(e.clientX<r.left||e.clientX>r.right||e.clientY<r.top||e.clientY>r.bottom)e.target.close();}});
$('bt-dialog').addEventListener('close',()=>$('bluetooth').focus());
document.addEventListener('keydown',e=>{if(e.code==='Space'&&!e.repeat&&!$('bt-dialog').open&&![ 'INPUT','BUTTON'].includes(e.target.tagName)){e.preventDefault();dispatch('PLAY_TOGGLE');}});
const canvas=$('visualizer'),ctx=canvas.getContext('2d');
function drawVisualizer(){
 if(!ctx)return;const w=canvas.width,h=canvas.height;ctx.clearRect(0,0,w,h);
 const p=animationPhase,amplitude=1;
 const gradient=ctx.createLinearGradient(0,0,w,0);['#ed57c6','#f59363','#e6dd56','#8cd363','#4cddd5','#5a92fa','#b96aef'].forEach((c,i)=>gradient.addColorStop(i/6,c));
 ctx.strokeStyle='#ffffff10';ctx.lineWidth=1;for(let x=12;x<w;x+=28){ctx.beginPath();ctx.moveTo(x,6);ctx.lineTo(x,h-18);ctx.stroke();}
 ctx.fillStyle=gradient;ctx.strokeStyle=gradient;
 const mode=state.visualization;
 if(mode==='spectrum'){
  const n=56,gap=5,bw=(w-(n-1)*gap)/n,base=h-26;
  for(let i=0;i<n;i++){
   const envelope=.3+.7*Math.pow(Math.sin(i*.14+1),2),pulse=.4+.6*Math.abs(Math.sin(p*2+i*.47)*Math.cos(p*.72+i*.23));
   const boost=state.fx==='bass'?(i<15?1.5:.65):state.fx==='space'?1+.25*Math.sin(i*.6):state.fx==='retro'?.65:1;
   let bh=Math.max(5,Math.min(base-8,amplitude*(28+112*envelope*pulse)*boost));if(state.fx==='retro')bh=Math.round(bh/12)*12+4;
   ctx.globalAlpha=1;ctx.fillRect(i*(bw+gap),base-bh,bw,bh);ctx.globalAlpha=.13;ctx.fillRect(i*(bw+gap),base+5,bw,Math.min(18,bh*.18));
  }ctx.globalAlpha=.35;ctx.fillRect(0,base+2,w,1);
 }else if(mode==='wave'){
  for(let j=0;j<3;j++){ctx.globalAlpha=j===0?.9:.25;ctx.lineWidth=j===0?3:1.5;ctx.beginPath();for(let x=0;x<w;x+=3){let y=h/2+Math.sin(x*.023+p*2+j*.4)*Math.sin(x*.0055+p*.2)*60*amplitude*(state.fx==='bass'?1.25:1);if(state.fx==='retro')y=Math.round(y/14)*14;x?ctx.lineTo(x,y):ctx.moveTo(x,y);}ctx.stroke();}
 }else{
  for(let i=0;i<62;i++){const a=i*2.399+p*.15,r=(18+(i%16)*4.5)*amplitude,x=w/2+Math.cos(a)*r*2.6,y=h/2+Math.sin(a)*r;ctx.globalAlpha=.35+(i%5)*.13;ctx.beginPath();ctx.arc(x,y,1.4+(i%4)*.65,0,Math.PI*2);ctx.fill();}
 }ctx.globalAlpha=1;
}
function drawDragon(){
 if(!dragonRenderer)return;
 try{dragonRenderer.draw({time:dragonPhase,variant:state.settings.dragon.type==='flight'?2:3,colors:state.settings.dragon.colors});}
 catch{dragonFallback();}
}
function dragonFallback(){
 dragonRenderer=null;$('dragon-canvas').hidden=true;$('dragon-fallback').hidden=false;$('dragon-render-note').hidden=false;
}
function initDragon(){
 const image=$('dragon-fallback');
 const ready=()=>{try{
  dragonRenderer=window.Tab5Dragon.create($('dragon-canvas'),image,{fit:1,minPixelRatio:2,onError:dragonFallback});
  $('dragon-canvas').hidden=false;drawDragon();
  if(dragonRenderer){image.hidden=true;$('dragon-render-note').hidden=true;}
 }catch{dragonFallback();}};
 if(image.complete&&image.naturalWidth)ready();else{image.addEventListener('load',ready,{once:true});image.addEventListener('error',dragonFallback,{once:true});}
}
function frame(now){const dt=Math.min((now-lastFrame)/1000,1);lastFrame=now;if(!document.hidden){if(state.playing&&!scrubbing)dispatch('TICK',dt);if(state.settings.animations&&!reducedMotion.matches){if(state.playing)animationPhase+=dt;dragonPhase+=Math.min(dt,.1)*state.settings.dragon.speed;}if(now-lastPaint>50){drawVisualizer();drawDragon();lastPaint=now;}}requestAnimationFrame(frame);}
document.addEventListener('visibilitychange',()=>{lastFrame=performance.now();});
function resize(){const v=$('viewport'),cs=getComputedStyle(v),w=v.clientWidth-parseFloat(cs.paddingLeft)-parseFloat(cs.paddingRight),h=window.innerHeight-parseFloat(cs.paddingTop)-parseFloat(cs.paddingBottom),scale=Math.max(.25,Math.min(1,w/1024,h/600));$('stage').style.width=`${1024*scale}px`;$('stage').style.height=`${600*scale}px`;$('device').style.transform=`scale(${scale})`;}
window.addEventListener('resize',resize);
function clock(){try{$('clock').textContent=new Intl.DateTimeFormat('en-GB',{hour:'2-digit',minute:'2-digit',hour12:!state.settings.clock24,timeZone:'Europe/Warsaw'}).format(new Date());}catch{$('clock').textContent='14:28';}}
clock();setInterval(clock,30000);render();resize();initDragon();requestAnimationFrame(frame);
// Integration inspection only: snapshots are copies; use real engine events in firmware.
window.tab5Demo=Object.freeze({getState:()=>JSON.parse(JSON.stringify(state)),tracks:()=>JSON.parse(JSON.stringify(M.tracks)),dispatch:(type,payload)=>dispatch(type,payload)});
})();
