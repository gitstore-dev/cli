// Derived from GitStore b6c866421dbcf43994565f847553c1f6aec83994 public schema.
const http = require('http');
const objects = new Map();
let calls = [];
http.createServer(async (req, res) => {
  const reply = (code, value) => { res.writeHead(code, {'content-type':'application/json'}); res.end(JSON.stringify(value)); };
  if (req.url === '/calls') return reply(200, calls);
  if (req.method !== 'POST' || !req.url.startsWith('/graphql')) return reply(501, {error:'Outside fixture contract'});
  let body = req.body;
  if (body === undefined) { let raw=''; for await (const part of req) raw+=part; try { body=JSON.parse(raw); } catch { return reply(400,{error:'Invalid JSON'}); } }
  if (typeof body === 'string') { try { body=JSON.parse(body); } catch { return reply(400,{error:'Invalid JSON'}); } }
  calls.push({query:body.query,variables:body.variables}); // Deliberately exclude authorization.
  if (req.url.endsWith('/unauthorized')) return reply(401,{});
  if (req.url.endsWith('/denied')) return reply(403,{});
  if (req.url.endsWith('/rate')) { res.setHeader('retry-after','0'); return reply(429,{}); }
  if (req.url.endsWith('/partial')) return reply(200,{data:{example:null},errors:[{message:'denied',extensions:{code:'FORBIDDEN'}}],extensions:{trace:'fixture'}});
  if (req.url.endsWith('/disconnect')) return req.socket.destroy();
  if (req.url.endsWith('/timeout')) return setTimeout(()=>reply(200,{data:{ok:true}}),2000);
  if (req.url.endsWith('/malformed')) {res.end('not-json');return;}
  if (req.url.endsWith('/oversized')) {res.end('x'.repeat(16*1024*1024+1));return;}
  const q=body.query||'', v=body.variables||{};
  const single = q.match(/\b(namespace|repository|product|category|collection|file)\s*\(/);
  const list = q.match(/\b(namespaces|repositories|products|categories|collections)\s*\(/);
  const kinds={namespace:'Namespace',repository:'Repository',product:'Product',category:'CategoryTaxonomy',collection:'Collection',file:'File'};
  if (q.includes('createProduct')||q.includes('createCategory')) {
    const field=q.includes('createProduct')?'product':'category', input=v.input;
    if (input.metadata.name==='fail-fixture') return reply(200,{data:{[field==='product'?'createProduct':'createCategory']:{[field]:null}},errors:[{message:'fixture authorization rejection',extensions:{code:'FORBIDDEN'}}]});
    const object={id:'opaque-fixture-id',...input,metadata:{...input.metadata,revision:'fixture-commit',uid:'fixture',resourceVersion:'1',generation:1}};
    objects.set(input.metadata.name,object);return reply(200,{data:{[field==='product'?'createProduct':'createCategory']:{[field]:object}}});
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
