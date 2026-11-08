// Derived from GitStore b6c866421dbcf43994565f847553c1f6aec83994 public schema.
const http = require('http');
const objects = new Map();
let calls = [];
const sessions = new Set();
let sessionCounter = 0;
http.createServer(async (req, res) => {
  const reply = (code, value) => { res.writeHead(code, {'content-type':'application/json'}); res.end(JSON.stringify(value)); };
  if (req.url === '/calls') return reply(200, calls);
  if (req.method !== 'POST' || !(req.url.startsWith('/graphql') || req.url === '/admin/graphql')) return reply(501, {error:'Outside fixture contract'});
  let body = req.body;
  if (body === undefined) { let raw=''; for await (const part of req) raw+=part; try { body=JSON.parse(raw); } catch { return reply(400,{error:'Invalid JSON'}); } }
  if (typeof body === 'string') { try { body=JSON.parse(body); } catch { return reply(400,{error:'Invalid JSON'}); } }
  const authInput = /\b(login|refreshToken)\s*\(/.test(body.query||'');
  calls.push({query:body.query,variables:authInput?{credentialInput:'redacted'}:body.variables}); // Never record authorization, passwords or refresh tokens.
  if (req.url.endsWith('/unauthorized')) return reply(401,{});
  if (req.url.endsWith('/denied')) return reply(403,{});
  if (req.url.endsWith('/rate')) { res.setHeader('retry-after','0'); return reply(429,{}); }
  if (req.url.endsWith('/empty-errors')) return reply(200,{errors:[]});
  if (req.url.endsWith('/partial')) return reply(200,{data:{example:null},errors:[{message:'denied',extensions:{code:'FORBIDDEN'}}],extensions:{trace:'fixture'}});
  if (req.url.endsWith('/disconnect')) return req.socket.destroy();
  if (req.url.endsWith('/timeout')) return setTimeout(()=>reply(200,{data:{ok:true}}),2000);
  if (req.url.endsWith('/malformed')) {res.end('not-json');return;}
  if (req.url.endsWith('/oversized')) {res.end('x'.repeat(16*1024*1024+1));return;}
  const q=body.query||'', v=body.variables||{};
  const tokenPayload = (field, token) => ({data:{[field]:{token:{accessToken:token,refreshToken:token,tokenType:'Bearer',expiresIn:3600,scope:null,idToken:null}}}});
  if (/\blogin\s*\(/.test(q)) {
    if (req.headers.authorization || v.input?.username!=='synthetic-user' || v.input?.password!=='synthetic-password')
      return reply(200,{errors:[{message:'invalid synthetic credentials'}]});
    const token='synthetic-session-'+(++sessionCounter); sessions.add(token);
    return reply(200,tokenPayload('login',token));
  }
  if (/\brefreshToken\s*\(/.test(q)) {
    if (req.headers.authorization || !sessions.delete(v.input?.refreshToken))
      return reply(200,{errors:[{message:'session revoked or unavailable'}]});
    const token='synthetic-session-'+(++sessionCounter); sessions.add(token);
    return reply(200,tokenPayload('refreshToken',token));
  }
  if (/\blogout\s*\{/.test(q)) {
    const token=(req.headers.authorization||'').replace(/^Bearer /,'');
    return reply(200,{data:{logout:{success:sessions.delete(token)}}});
  }
  const single = q.match(/\b(namespace|repository|product|category|collection|file)\s*\(/);
  const list = q.match(/\b(namespaces|repositories|products|categories|collections)\s*\(/);
  if (req.url === '/admin/graphql' && single?.[1] === 'file') {
    if (!q.includes('file(by:$by)') || !v.by || Boolean(v.by.namespacePath) === Boolean(v.by.id))
      return reply(200,{errors:[{message:'FileBy selector required'}]});
    if (v.by.namespacePath?.name === 'absent' || v.by.id === 'absent') return reply(200,{data:{file:null}});
  }
  const kinds={namespace:'Namespace',repository:'Repository',product:'Product',category:'CategoryTaxonomy',collection:'Collection',file:'File'};
  const create = q.match(/\bcreate(Namespace|Repository|Product|Category)\s*\(/);
  if (create) {
    const field=create[1].toLowerCase(), input=v.input;
    if (['namespace','repository'].includes(field) && ('body' in input || 'status' in input))
      return reply(200,{errors:[{message:'Unsupported authoring input'}]});
    if (input.metadata.name==='fail-fixture') return reply(200,{data:{[field==='product'?'createProduct':'createCategory']:{[field]:null}},errors:[{message:'fixture authorization rejection',extensions:{code:'FORBIDDEN'}}]});
    const object={id:'opaque-fixture-id',...input,metadata:{...input.metadata,revision:'fixture-commit',uid:'fixture',resourceVersion:'1',generation:1}};
    objects.set(input.metadata.name,object);return reply(200,{data:{['create'+create[1]]:{[field]:object}}});
  }
  const del=q.match(/\bdelete(Namespace|Repository|Product|Category)\s*\(/);
  if (del) {const field=del[1].toLowerCase();return reply(200,{data:{['delete'+del[1]]:{[field]:null,outcome:'TERMINATION_STARTED'}}});}
  if (list) return reply(200,{data:{[list[1]]:{edges:[],pageInfo:{hasNextPage:false,hasPreviousPage:false,startCursor:null,endCursor:null}}}});
  if (single) {
    const field=single[1], name=v.by?.namespacePath?.name||v.by?.name||v.name||'fixture';
    return reply(200,{data:{[field]:objects.get(name)||{id:'opaque-fixture-id',kind:kinds[field],apiVersion:'catalog.gitstore.dev/v1beta1',metadata:{name,namespace:'shop',revision:null,uid:'fixture',resourceVersion:'1',generation:1},body:'fixture body\n',spec:{title:'Fixture'}}}});
  }
  return reply(200,{data:{echo:v},extensions:{source:'controlled-fixture'}});
}).listen(process.env.PORT);
