const enc = new TextEncoder();
const dec = new TextDecoder();
const now = () => Math.floor(Date.now()/1000);
const json = (x, status=200) => new Response(JSON.stringify(x), {status, headers:{'content-type':'application/json; charset=utf-8','cache-control':'no-store'}});
const html = (x) => new Response(x,{headers:{'content-type':'text/html; charset=utf-8','cache-control':'no-store'}});

async function sha256(s){const b=await crypto.subtle.digest('SHA-256',enc.encode(s));return [...new Uint8Array(b)].map(x=>x.toString(16).padStart(2,'0')).join('');}
async function randomToken(){return crypto.randomUUID()+crypto.randomUUID().replaceAll('-','');}
async function hashPassword(password,salt){return sha256(`${salt}:${password}`);}
async function passwordRecord(password){const salt=await randomToken();return `${salt}$${await hashPassword(password,salt)}`;}
async function verifyPassword(password,record){const [salt,h]=String(record).split('$');return !!salt&&h===await hashPassword(password,salt);}
async function body(req){try{return await req.json()}catch{return null}}
function bearer(req){const h=req.headers.get('authorization')||'';return h.startsWith('Bearer ')?h.slice(7):'';}
async function sessionUser(env,req){const t=bearer(req);if(!t)return null;const h=await sha256(t);return env.DB.prepare('SELECT u.* FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.token_hash=? AND s.expires_at>?').bind(h,now()).first();}
async function deviceToken(env,req){const t=bearer(req);if(!t)return null;const h=await sha256(t);return env.DB.prepare('SELECT * FROM devices WHERE token_hash=?').bind(h).first();}
async function canAccess(env,user,deviceId,needed='viewer'){if(!user)return false;if(user.role==='admin')return true;const r=await env.DB.prepare('SELECT role FROM device_users WHERE device_id=? AND user_id=?').bind(deviceId,user.id).first();if(!r)return false;const rank={viewer:0,operator:1,admin:2};return (rank[r.role]??-1)>=(rank[needed]??99);}

async function bootstrap(env){
  const count=await env.DB.prepare('SELECT COUNT(*) c FROM users').first();
  if((count?.c||0)>0)return;
  if(!env.ADMIN_EMAIL||!env.ADMIN_PASSWORD)throw new Error('Set ADMIN_EMAIL and ADMIN_PASSWORD secrets before first request.');
  const rec=await passwordRecord(env.ADMIN_PASSWORD);
  await env.DB.prepare('INSERT INTO users(email,password_hash,role,created_at) VALUES(?,?,?,?)').bind(env.ADMIN_EMAIL.toLowerCase(),rec,'admin',now()).run();
}

async function api(env,req,url){
  await bootstrap(env);
  const p=url.pathname;
  if(p==='/api/auth/login'&&req.method==='POST'){
    const b=await body(req);if(!b?.email||!b?.password)return json({error:'email and password required'},400);
    const u=await env.DB.prepare('SELECT * FROM users WHERE email=?').bind(String(b.email).toLowerCase()).first();
    if(!u||!(await verifyPassword(b.password,u.password_hash)))return json({error:'invalid credentials'},401);
    const t=await randomToken(),ttl=Number(env.SESSION_TTL_SECONDS||604800);await env.DB.prepare('INSERT INTO sessions(token_hash,user_id,expires_at) VALUES(?,?,?)').bind(await sha256(t),u.id,now()+ttl).run();
    return json({token:t,user:{id:u.id,email:u.email,role:u.role}});
  }
  if(p==='/api/auth/logout'&&req.method==='POST'){const t=bearer(req);if(t)await env.DB.prepare('DELETE FROM sessions WHERE token_hash=?').bind(await sha256(t)).run();return json({ok:true});}
  if(p==='/api/auth/me'&&req.method==='GET'){const u=await sessionUser(env,req);return u?json({user:{id:u.id,email:u.email,role:u.role}}):json({error:'unauthorized'},401);}

  const u=await sessionUser(env,req);
  if(p==='/api/admin/users'&&req.method==='POST'){
    if(!u||u.role!=='admin')return json({error:'forbidden'},403);const b=await body(req);if(!b?.email||!b?.password)return json({error:'email and password required'},400);const rec=await passwordRecord(b.password);try{const r=await env.DB.prepare('INSERT INTO users(email,password_hash,role,created_at) VALUES(?,?,?,?)').bind(String(b.email).toLowerCase(),rec,b.role==='viewer'?'viewer':'member',now()).run();return json({ok:true,id:r.meta.last_row_id});}catch{return json({error:'user already exists'},409);}
  }
  if(p==='/api/admin/devices'&&req.method==='POST'){
    if(!u||u.role!=='admin')return json({error:'forbidden'},403);const b=await body(req);const id=String(b?.id||'').trim(),name=String(b?.name||id).trim();if(!/^[A-Za-z0-9_-]{3,63}$/.test(id)||!name)return json({error:'invalid device id/name'},400);const token=await randomToken();try{await env.DB.prepare('INSERT INTO devices(id,name,owner_id,token_hash,created_at) VALUES(?,?,?,?,?)').bind(id,name,u.id,await sha256(token),now()).run();await env.DB.prepare('INSERT OR REPLACE INTO device_users(device_id,user_id,role) VALUES(?,?,?)').bind(id,u.id,'admin').run();return json({ok:true,id,name,token});}catch{return json({error:'device already exists'},409);}
  }
  if(p==='/api/admin/grant'&&req.method==='POST'){
    if(!u||u.role!=='admin')return json({error:'forbidden'},403);const b=await body(req);if(!b?.deviceId||!b?.email)return json({error:'deviceId and email required'},400);const x=await env.DB.prepare('SELECT id FROM users WHERE email=?').bind(String(b.email).toLowerCase()).first();if(!x)return json({error:'user not found'},404);await env.DB.prepare('INSERT OR REPLACE INTO device_users(device_id,user_id,role) VALUES(?,?,?)').bind(b.deviceId,x.id,b.role==='viewer'?'viewer':'operator').run();return json({ok:true});
  }

  if(p==='/api/devices'&&req.method==='GET'){
    if(!u)return json({error:'unauthorized'},401);const cutoff=now()-15;const rows=u.role==='admin'?await env.DB.prepare('SELECT id,name,(last_seen>? ) online,last_seen,states,enabled FROM devices ORDER BY name').bind(cutoff).all():await env.DB.prepare('SELECT d.id,d.name,(d.last_seen>? ) online,d.last_seen,d.states,d.enabled FROM devices d JOIN device_users du ON du.device_id=d.id WHERE du.user_id=? ORDER BY d.name').bind(cutoff,u.id).all();return json({devices:rows.results||[]});
  }
  const dm=p.match(/^\/api\/devices\/([^/]+)$/);if(dm&&req.method==='GET'){
    if(!u)return json({error:'unauthorized'},401);const id=decodeURIComponent(dm[1]);if(!(await canAccess(env,u,id,'viewer')))return json({error:'forbidden'},403);const d=await env.DB.prepare('SELECT id,name,(last_seen>? ) online,last_seen,states,enabled FROM devices WHERE id=?').bind(now()-15,id).first();return d?json({device:d}):json({error:'not found'},404);
  }
  const cm=p.match(/^\/api\/devices\/([^/]+)\/relay$/);if(cm&&req.method==='POST'){
    if(!u)return json({error:'unauthorized'},401);const id=decodeURIComponent(cm[1]);if(!(await canAccess(env,u,id,'operator')))return json({error:'forbidden'},403);const b=await body(req),relay=Number(b?.relay),state=Number(b?.state);if(relay<1||relay>5||![0,1].includes(state))return json({error:'invalid relay/state'},400);await env.DB.prepare('INSERT INTO commands(device_id,relay,state,created_at) VALUES(?,?,?,?)').bind(id,relay,state,now()).run();return json({ok:true});
  }
  const sm=p.match(/^\/api\/devices\/([^/]+)\/schedules$/);if(sm&&req.method==='GET'){
    if(!u)return json({error:'unauthorized'},401);const id=decodeURIComponent(sm[1]);if(!(await canAccess(env,u,id,'viewer')))return json({error:'forbidden'},403);const r=await env.DB.prepare('SELECT id,relay,hour,minute,action,days,enabled FROM schedules WHERE device_id=? ORDER BY id').bind(id).all();return json({schedules:r.results||[]});
  }
  if(sm&&req.method==='POST'){
    if(!u)return json({error:'unauthorized'},401);const id=decodeURIComponent(sm[1]);if(!(await canAccess(env,u,id,'operator')))return json({error:'forbidden'},403);const b=await body(req);if(!Array.isArray(b?.schedules)||b.schedules.length>20)return json({error:'maximum 20 schedules'},400);await env.DB.prepare('DELETE FROM schedules WHERE device_id=?').bind(id).run();for(const s of b.schedules){if(Number(s.relay)<1||Number(s.relay)>5||Number(s.hour)<0||Number(s.hour)>23||Number(s.minute)<0||Number(s.minute)>59)continue;await env.DB.prepare('INSERT INTO schedules(device_id,relay,hour,minute,action,days,enabled) VALUES(?,?,?,?,?,?,?)').bind(id,Number(s.relay),Number(s.hour),Number(s.minute),Number(s.action)?1:0,Number(s.days)||127,s.enabled?1:0).run();}return json({ok:true});
  }
  const om=p.match(/^\/api\/devices\/([^/]+)\/ota$/);if(om&&req.method==='POST'){
    if(!u)return json({error:'unauthorized'},401);const id=decodeURIComponent(om[1]);if(!(await canAccess(env,u,id,'admin')))return json({error:'forbidden'},403);const b=await body(req);const url=String(b?.url||'');if(!url.startsWith('https://'))return json({error:'OTA URL must use HTTPS'},400);await env.DB.prepare('UPDATE devices SET ota_url=? WHERE id=?').bind(url,id).run();return json({ok:true});
  }
  const pr=p==='/api/device/poll';if(pr&&req.method==='POST'){
    const d=await deviceToken(env,req);if(!d)return json({error:'unauthorized'},401);const b=await body(req);if(b?.deviceId!==d.id)return json({error:'device id mismatch'},403);const states=Array.isArray(b.states)?b.states.slice(0,5).map(x=>x?1:0):[0,0,0,0,0];const enabled=Array.isArray(b.enabled)?b.enabled.slice(0,5).map(Boolean):[true,true,true,false,false];await env.DB.prepare('UPDATE devices SET online=1,last_seen=?,states=?,enabled=? WHERE id=?').bind(now(),JSON.stringify(states),JSON.stringify(enabled),d.id).run();const cmds=await env.DB.prepare('SELECT id,relay,state FROM commands WHERE device_id=? ORDER BY id LIMIT 20').bind(d.id).all();if(cmds.results?.length)await env.DB.prepare(`DELETE FROM commands WHERE id IN (${cmds.results.map(()=>'?').join(',')})`).bind(...cmds.results.map(x=>x.id)).run();const sch=await env.DB.prepare('SELECT id,relay,hour,minute,action,days,enabled FROM schedules WHERE device_id=? ORDER BY id LIMIT 20').bind(d.id).all();const od=await env.DB.prepare('SELECT ota_url FROM devices WHERE id=?').bind(d.id).first();if(od?.ota_url)await env.DB.prepare('UPDATE devices SET ota_url=NULL WHERE id=?').bind(d.id);return json({commands:cmds.results||[],schedules:sch.results||[],ota:od?.ota_url?{url:od.ota_url}:null});
  }
  return null;
}

export default {async fetch(req,env){const url=new URL(req.url);try{const r=await api(env,req,url);if(r)return r;return env.ASSETS?env.ASSETS.fetch(req):new Response('Not found',{status:404});}catch(e){return json({error:e.message||'server error'},500);}}};
