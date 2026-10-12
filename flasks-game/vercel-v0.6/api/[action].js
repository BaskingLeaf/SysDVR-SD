import {put,get} from '@vercel/blob';
import crypto from 'node:crypto';

const person=name=>({name:String(name||'Writer').slice(0,24),token:crypto.randomUUID(),points:5,seals:0,credit:0});
const respond=(res,obj,status=200)=>res.status(status).json(obj);
const append=(r,kind,data={})=>r.events.push({kind,...data});
function migrate(r){
  if(!Array.isArray(r.layers)){
    r.layers=(r.parents||[null]).map((parent,id)=>({id,parent,createdBy:id===0?'Origin':'An earlier writer'}));
  }
  if(!Array.isArray(r.events)){
    r.events=(r.log||[]).map(item=>{
      const [layer,by,text]=item;
      const kind=text==='['?'open':text===']'?'close':text?.startsWith('] ')?'seal':'text';
      return {kind,layer,by,text,parent:r.layers[layer]?.parent??null,reason:kind==='seal'?text.slice(2):undefined};
    });
  }
  if(typeof r.layer!=='number'||!r.layers[r.layer])r.layer=0;
  if(!Array.isArray(r.players))throw Error('Damaged player list');
  for(const p of r.players){p.credit??=0;p.points??=5;p.seals??=0;}
  r.draft??='';r.turn??=1;r.until??=0;r.window??=0;r.result??='';r.rematches??=0;
  return r;
}
async function load(code){
  try{
    const file=await get('rooms/'+code+'.json',{access:'private',useCache:false});
    if(!file||file.statusCode!==200)return null;
    return migrate(JSON.parse(await new Response(file.stream).text()));
  }catch(err){console.error('Room load failed:',err);return null;}
}
async function save(r){
  await put('rooms/'+r.code+'.json',JSON.stringify(r),{
    access:'private',allowOverwrite:true,addRandomSuffix:false,
    contentType:'application/json',cacheControlMaxAge:60
  });
}
function commitDraft(r){
  if(r.draft){append(r,'text',{layer:r.layer,by:r.players[r.active].name,text:r.draft});r.draft='';}
}
function seal(r,receiver,reason){
  commitDraft(r);
  append(r,'seal',{layer:r.layer,by:r.players[receiver].name,reason});
  r.players[r.active].seals++;
  r.active=receiver;r.turn++;r.until=Date.now()+11000;r.window=0;
  // A turn handoff does not close a reality. Explicit ] alone does that.
  if(r.players[1-receiver].seals>=5){r.ended=true;r.result=r.players[receiver].name+' wins';}
}
function serialise(r,p){
  const i=r.players.indexOf(p),writer=r.active===i;
  return {
    room:r.code,players:r.players.map(x=>({name:x.name,points:x.points,seals:x.seals})),
    me:i,active:r.active,turn:r.turn,layer:r.layer,
    layers:r.layers,events:r.events,
    draft:writer?r.draft:Array.from(r.draft).slice(0,-4).join(''),
    canInterrupt:Date.now()<r.window,remaining:Math.max(0,r.until-Date.now()),
    started:r.started,ended:r.ended,result:r.result
  };
}
function transcript(r){
  let lines=['FLASKS AND GODS · NESTED SCRIPTURE'];
  for(const e of r.events){
    const depth=(()=>{let d=0,id=e.layer;for(let k=0;k<16&&r.layers[id]?.parent!==null;k++){d++;id=r.layers[id].parent;}return d})();
    const prefix='  '.repeat(depth);
    if(e.kind==='open')lines.push(prefix+'[ Layer '+e.layer+' created by '+e.by+' (inside '+e.parent+')');
    else if(e.kind==='enter')lines.push(prefix+'[ Re-enter Layer '+e.layer+' — '+e.by);
    else if(e.kind==='close')lines.push(prefix+'] Exit Layer '+e.layer+' → '+e.parent+' ('+e.by+')');
    else if(e.kind==='seal')lines.push(prefix+'⟧ '+e.by+' gains the quill ('+e.reason+')');
    else if(e.kind==='text')lines.push(prefix+e.by+' (Layer '+e.layer+'): '+e.text);
  }
  if(r.draft)lines.push('[Unfinished, Layer '+r.layer+'] '+r.players[r.active].name+': '+r.draft);
  return lines.join('\n')+'\n\nPlease evaluate the writing, how the nested realities were used, and the fairness of turn changes.';
}
export default async function handler(req,res){
  res.setHeader('Cache-Control','no-store');
  if(req.method!=='POST')return respond(res,{error:'Use POST'},405);
  try{
    const body=req.body||{},action=String(req.query.action||'');
    if(action==='create'){
      let code=crypto.randomBytes(4).toString('hex').slice(0,5).toUpperCase();
      while(await load(code))code=crypto.randomBytes(4).toString('hex').slice(0,5).toUpperCase();
      const p=person(body.name);
      const r={code,players:[p],started:false,ended:false,active:0,turn:1,layer:0,
        layers:[{id:0,parent:null,createdBy:'Origin'}],events:[],draft:'',window:0,
        until:0,created:Date.now(),result:'',rematches:0};
      await save(r);return respond(res,{room:code,token:p.token});
    }
    const room=await load(String(body.room||'').toUpperCase());
    if(!room)return respond(res,{error:'Room unavailable. Try making a fresh room.'},404);
    if(action==='join'){
      const same=room.players.find(p=>p.token===body.token);
      if(same)return respond(res,{room:room.code,token:same.token});
      if(room.players.length>=2)return respond(res,{error:'Room is full. Ask for a fresh invitation.'},409);
      const p=person(body.name);room.players.push(p);await save(room);return respond(res,{room:room.code,token:p.token});
    }
    const p=room.players.find(x=>x.token===body.token);
    if(!p)return respond(res,{error:'Invalid session'},403);
    const i=room.players.indexOf(p);
    if(room.started&&!room.ended&&room.until&&Date.now()>room.until){seal(room,1-room.active,'silence');await save(room);}
    if(action==='state')return respond(res,serialise(room,p));
    if(action==='start'){
      if(i===0&&room.players.length===2&&!room.started){room.started=true;room.until=Date.now()+11000;await save(room);}
      return respond(res,{ok:true});
    }
    if(action==='rematch'){
      if(i!==0)return respond(res,{error:'Only the host can restart the match'},403);
      if(room.players.length!==2)return respond(res,{error:'Need two players first'},400);
      room.rematches++;
      room.started=true;room.ended=false;room.result='';room.active=room.rematches%2;
      room.turn++;room.layer=0;room.layers=[{id:0,parent:null,createdBy:'Origin'}];
      room.events=[];room.draft='';room.window=0;room.until=Date.now()+11000;
      for(const x of room.players){x.points=5;x.seals=0;x.credit=0;}
      await save(room);return respond(res,{ok:true});
    }
    if(action==='export')return respond(res,{text:transcript(room)});
    if(!room.started||room.ended)return respond(res,{ok:true});
    if(action==='type'&&room.active===i){
      const next=Array.from(String(body.text||'')).slice(0,1500).join(''),old=room.draft;
      if(next!==old){
        room.draft=next;room.until=Date.now()+6500;
        const other=room.players[1-i];
        other.credit+=Math.max(0,Array.from(next).length-Array.from(old).length);
        while(other.credit>=18){other.credit-=18;other.points=Math.min(12,other.points+1);}
        const shown=Array.from(next).slice(0,-4).join(''),prev=Array.from(old).slice(0,-4).join('');
        if(shown.startsWith(prev)&&Array.from(shown.slice(prev.length)).some(ch=>/[\s.,!?;:]/.test(ch)))room.window=Date.now()+4000;
        await save(room);
      }
      return respond(res,{ok:true});
    }
    if(action==='action'){
      if(body.action==='interrupt'&&i!==room.active&&p.points>=3){
        p.points-=3;
        if(Date.now()<room.window){p.points++;seal(room,i,'timed interruption');}
        await save(room);return respond(res,{ok:true});
      }
      if(i!==room.active)return respond(res,{error:'You do not currently hold the quill'},409);
      if(body.action==='yield')seal(room,1-i,'yielded');
      else if(body.action==='open'){
        if(p.points<2)return respond(res,{error:'Opening a layer costs 2 points'},400);
        if(room.layers.length>=16)return respond(res,{error:'Maximum 16 realities per duel'},400);
        p.points-=2;commitDraft(room);
        const parent=room.layer,id=room.layers.length;
        room.layers.push({id,parent,createdBy:p.name});
        append(room,'open',{layer:id,parent,by:p.name});
        room.layer=id;room.until=Date.now()+11000;
      }else if(body.action==='close'){
        if(room.layer===0)return respond(res,{error:'You are already at the origin'},400);
        if(p.points<2)return respond(res,{error:'Closing a layer costs 2 points'},400);
        p.points-=2;commitDraft(room);
        const layer=room.layer,parent=room.layers[layer].parent;
        append(room,'close',{layer,parent,by:p.name});
        room.layer=parent;room.until=Date.now()+11000;
      }else if(body.action==='enter'){
        const target=Number(body.target);
        if(!Number.isInteger(target)||!room.layers[target]||room.layers[target].parent!==room.layer)return respond(res,{error:'You can only revisit a direct child reality'},400);
        if(p.points<2)return respond(res,{error:'Re-entering costs 2 points'},400);
        p.points-=2;commitDraft(room);
        append(room,'enter',{layer:target,parent:room.layer,by:p.name});
        room.layer=target;room.until=Date.now()+11000;
      }else return respond(res,{error:'Unknown action'},404);
      await save(room);return respond(res,{ok:true});
    }
    return respond(res,{error:'Unknown route'},404);
  }catch(err){console.error('Game API error:',err);return respond(res,{error:'Server error: '+err.message},500);}
}