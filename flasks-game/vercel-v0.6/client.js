'use strict';
const $=id=>document.getElementById(id),STORAGE='fg-phone';
let session=null,state=null,lastKey='',pending=null,busy=false,pollBusy=false,lastName='',lastError='';
const visible=(id,value)=>$(id).classList.toggle('hidden',!value);
const msg=e=>{$('error').textContent=e?.message||String(e);};
function saved(){try{const s=JSON.parse(localStorage.getItem(STORAGE));return s?.room&&s?.token?s:null;}catch{return null;}}
function resumeLabel(){const s=saved();visible('resumeBox',!!s);if(s)$('resume').textContent='Resume room '+s.room;}
async function api(action,options={}){
  const r=await fetch('/api/'+action,{method:'POST',cache:'no-store',headers:{'Content-Type':'application/json'},body:JSON.stringify({...options,...session})});
  const j=await r.json();if(!r.ok)throw Error(j.error||'Network error');return j;
}
function menu(forget=false){
  if(state?.players?.[state.me]?.name)lastName=state.players[state.me].name;
  session=null;state=null;lastKey='';pending=null;clearTimeout(window.writeTimer);
  if(forget)localStorage.removeItem(STORAGE);
  $('name').value=$('name').value||lastName;
  history.replaceState(null,'',location.pathname);
  visible('game',false);visible('lobby',true);resumeLabel();$('error').textContent='';
}
function enter(s){
  session=s;state=null;pending=null;lastKey='';
  localStorage.setItem(STORAGE,JSON.stringify(s));
  history.replaceState(null,'','?room='+encodeURIComponent(s.room));
  visible('lobby',false);visible('game',true);$('error').textContent='';poll();
}
async function resume(){
  const s=saved();if(!s)return;session=s;
  try{await api('state');enter(s);}
  catch(e){session=null;localStorage.removeItem(STORAGE);resumeLabel();msg('Saved room is unavailable. Create a new room.');}
}
async function createRoom(){try{lastName=$('name').value||lastName;enter(await api('create',{name:lastName}));}catch(e){msg(e);}}
async function joinRoom(){try{lastName=$('name').value||lastName;enter(await api('join',{room:$('code').value.trim().toUpperCase(),name:lastName}));}catch(e){msg(e);}}
async function poll(){
  if(!session||pollBusy)return;const active=session;pollBusy=true;
  try{const next=await api('state');if(session?.token!==active.token||session?.room!==active.room)return;state=next;render();}
  catch(e){if(session?.token===active.token){if(lastError!==e.message)msg(e);lastError=e.message;}}
  finally{pollBusy=false;}
}
function depth(layers,id){let d=0,n=layers[id];for(let j=0;n&&n.parent!==null&&j<20;j++){d++;n=layers[n.parent];}return d;}
function ancestry(layers,id){let a=[],n=layers[id];for(let j=0;n&&j<20;j++){a.unshift(n.id);n=n.parent===null?null:layers[n.parent];}return a;}
function renderTree(s,writing){
  const holder=$('realities');holder.replaceChildren();
  for(const item of s.layers){
    const row=document.createElement('div');row.className='reality'+(item.id===s.layer?' current':'');
    row.style.marginLeft=Math.min(52,depth(s.layers,item.id)*13)+'px';
    const left=document.createElement('div'),name=document.createElement('div'),subtitle=document.createElement('div');
    name.className='title';name.textContent=(item.id===s.layer?'◈ ':'')+(item.id===0?'Origin · Layer 0':'Reality '+item.id);
    subtitle.className='metadata';
    subtitle.textContent=item.parent===null?'Root of the scripture':'Child of '+item.parent+' · Created by '+item.createdBy;
    left.append(name,subtitle);row.append(left);
    if(writing&&item.parent===s.layer&&item.id!==s.layer){
      const button=document.createElement('button');button.className='secondary';
      button.textContent='[ Re-enter · 2';button.disabled=s.players[s.me].points<2;
      button.addEventListener('click',()=>act('enter',item.id));row.append(button);
    }
    holder.append(row);
  }
}
function appendEvent(holder,s,event,isDraft=false){
  const el=document.createElement('div'),level=depth(s.layers,event.layer)||0;
  el.style.marginLeft=Math.min(62,level*15)+'px';
  el.className='entry'+(event.kind==='text'?'':event.kind==='seal'?' seal':' bracket');
  const info=document.createElement('div');info.className='meta';
  info.textContent='Layer '+event.layer+' · '+event.by+(isDraft?' · typing':'');el.append(info);
  const text=document.createElement('div');
  if(event.kind==='text'){text.className='prose';text.textContent=event.text;}
  else if(event.kind==='open')text.textContent='[ Entered new reality '+event.layer+' from '+event.parent+' ]';
  else if(event.kind==='enter')text.textContent='[ Returned to existing reality '+event.layer+' ]';
  else if(event.kind==='close')text.textContent='] Left reality '+event.layer+' → '+event.parent;
  else if(event.kind==='seal')text.textContent='⟧ Quill transferred ('+event.reason+')';
  else text.textContent=event.text||'';
  el.append(text);holder.append(el);
}
function render(){
  const s=state;if(!s||!session)return;
  const me=s.players[s.me];if(!me)return;
  const writing=s.active===s.me,key=s.turn+':'+s.layer+':'+s.active;
  $('room').textContent='ROOM '+s.room;
  $('clock').textContent=s.started&&!s.ended?(s.remaining/1000).toFixed(1)+'s':'';
  $('scores').replaceChildren(...s.players.map((p,i)=>{
    const el=document.createElement('div');el.className='score'+(i===s.active?' active':'');
    const a=document.createElement('small');a.textContent=i===s.active?'GOD · WRITING':'FLASK · WATCHING';
    const b=document.createElement('b');b.textContent=p.name+(i===s.me?' · you':'');
    const c=document.createElement('div');c.className='hint';c.textContent=p.points+' points · '+p.seals+'/5 seals';
    el.append(a,b,c);return el;
  }));
  $('status').textContent=s.ended?s.result:!s.started?'Waiting for both writers':writing?'Your quill. You are writing in reality '+s.layer+'.':'You are watching reality '+s.layer+'.';
  $('layer').textContent='Layer '+s.layer+' / '+(s.layers.length-1)+' created';
  $('path').textContent=ancestry(s.layers,s.layer).map(id=>id===0?'Origin':'Reality '+id).join(' › ');
  renderTree(s,writing&&s.started&&!s.ended);
  const l=$('log'),priorScroll=l.scrollTop,followBottom=l.scrollHeight-l.clientHeight-l.scrollTop<60;
  l.replaceChildren();for(const e of s.events)appendEvent(l,s,e);
  if(s.draft)appendEvent(l,s,{kind:'text',layer:s.layer,by:s.players[s.active].name,text:s.draft},true);
  if(followBottom)l.scrollTop=l.scrollHeight;else l.scrollTop=priorScroll;
  visible('start',!s.started&&s.players.length===2&&s.me===0);
  visible('writer',s.started&&!s.ended&&writing);
  visible('watch',s.started&&!s.ended&&!writing);
  visible('rematch',s.started&&s.players.length===2&&s.me===0);
  visible('replayHint',s.ended&&s.me!==0);
  $('rematch').textContent=s.ended?'↻ Replay with same players':'↻ Restart this duel';
  if(key!==lastKey){lastKey=key;if(writing)$('draft').value=s.draft;pending=null;}
  $('writingAt').textContent='Layer '+s.layer;
  if(!writing)$('feed').textContent=s.draft||'The opponent has not written yet.';
  $('cue').textContent=s.canInterrupt?'SEAL WINDOW OPEN — STRIKE':'Wait for a new space or punctuation.';
  $('interrupt').disabled=me.points<3;
  $('open').disabled=me.points<2||s.layers.length>=16;
  $('close').disabled=me.points<2||s.layer===0;
}
async function push(){
  if(busy||pending===null||!session)return;
  busy=true;const text=pending;pending=null;
  try{await api('type',{text});}catch(e){msg(e);}
  finally{busy=false;if(pending!==null&&session)push();}
}
$('draft').addEventListener('input',()=>{
  pending=$('draft').value;clearTimeout(window.writeTimer);window.writeTimer=setTimeout(push,65);
});
async function flush(){clearTimeout(window.writeTimer);if(pending!==null)await push();while(busy)await new Promise(resolve=>setTimeout(resolve,30));}
async function act(action,target){
  if(!session)return;
  try{
    await flush();
    const route=['start','rematch'].includes(action)?action:'action';
    await api(route,route==='action'?{action,target}:{});
    lastKey='';await poll();
  }catch(e){msg(e);}
}
async function newRoom(){
  if(state?.players?.[state.me]?.name)lastName=state.players[state.me].name;
  menu(true);$('name').value=lastName;await createRoom();
}
async function copy(){
  try{
    const text=(await api('export')).text;
    if(navigator.clipboard?.writeText)await navigator.clipboard.writeText(text);
    else{const input=document.createElement('textarea');input.value=text;document.body.append(input);input.select();document.execCommand('copy');input.remove();}
    msg('Scripture copied. Paste it into ChatGPT.');
  }catch(e){msg(e);}
}
async function invite(){
  if(!session)return;const url=location.origin+'/?room='+session.room;
  try{
    if(navigator.share)await navigator.share({url,text:'Join Flasks & Gods — room '+session.room});
    else{await navigator.clipboard.writeText(url);msg('Invite copied');}
  }catch(e){if(e?.name!=='AbortError')msg(e);}
}
$('create').onclick=createRoom;$('join').onclick=joinRoom;$('resume').onclick=resume;
$('forget').onclick=()=>{localStorage.removeItem(STORAGE);resumeLabel();msg('Saved room forgotten.');};
$('menu').onclick=()=>menu(false);$('forgetInGame').onclick=()=>menu(true);$('newroom').onclick=newRoom;
$('share').onclick=invite;$('copy').onclick=copy;$('start').onclick=()=>act('start');
$('rematch').onclick=()=>{if(!state?.ended&&!confirm('Restart this match for both players?'))return;act('rematch');};
['open','close','yield','interrupt'].forEach(action=>$(action).onclick=()=>act(action));
const inviteCode=new URLSearchParams(location.search).get('room');
if(inviteCode)$('code').value=inviteCode.toUpperCase();
resumeLabel();setInterval(poll,450);
