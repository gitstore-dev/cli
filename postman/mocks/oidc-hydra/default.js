// Narrow controlled fixture derived from Hydra 2.3.0 at ee8c339ddada3a42529c0416897abc32bad03bbb.
// Failure injections are test controls, not claims of provider-specific errors.
const http = require('http');
const state = async () => (await pm.state.get('oidcHydra.clients')) || {};
const calls = async () => (await pm.state.get('oidcHydra.calls')) || [];
http.createServer(async (req, res) => {
  const reply = (code, value) => {
    res.writeHead(code, { 'content-type': 'application/json' });
    res.end(JSON.stringify(value));
  };
  if (req.method === 'GET' && req.url === '/__fixture/calls') return reply(200, await calls());
  if (req.method === 'POST' && req.url === '/__fixture/ready') {
    await pm.state.set('oidcHydra.ready', req.body);
    return reply(200, { fixture: true });
  }
  if (req.method === 'GET' && req.url === '/health/ready') {
    const selected = (await pm.state.get('oidcHydra.ready')) || { code: 200, status: 'ok' };
    return reply(selected.code, { status: selected.status });
  }
  const match = /^\/admin\/clients\/(gitstore-oidc-fixture-[a-z0-9-]+)$/.exec(req.url);
  if (req.method === 'GET' && match) {
    const observed = (await state())[match[1]];
    if (!observed) return reply(501, { fixture: 'Unknown-client response not modeled' });
    const value = { ...observed };
    if (match[1].endsWith('-expose-secret')) value.client_secret = 'synthetic-unexpected-public-secret';
    if (match[1].endsWith('-wrong-scope')) value.scope += ' admin';
    return reply(200, value);
  }
  if (req.method !== 'POST' || req.url !== '/admin/clients') {
    return reply(501, { fixture: 'Outside modeled consumer contract' });
  }
  let body = req.body;
  if (body === undefined) {
    let raw = '';
    for await (const chunk of req) {
      raw += chunk;
      if (raw.length > 8192) return reply(400, {});
    }
    try { body = JSON.parse(raw); } catch { return reply(400, {}); }
  }
  if (typeof body === 'string') {
    try { body = JSON.parse(body); } catch { return reply(400, {}); }
  }
  if (!body || typeof body !== 'object' || Array.isArray(body)) return reply(400, {});
  const fields = ['audience', 'client_id', 'client_secret', 'grant_types', 'redirect_uris',
    'response_types', 'scope', 'token_endpoint_auth_method'];
  if (JSON.stringify(Object.keys(body).sort()) !== JSON.stringify(fields) ||
      !/^gitstore-oidc-fixture-[a-z0-9-]+$/.test(body.client_id) ||
      !/^[A-Za-z0-9_-]{32,128}$/.test(body.client_secret) ||
      JSON.stringify(body.grant_types) !== JSON.stringify(['authorization_code', 'refresh_token']) ||
      JSON.stringify(body.response_types) !== JSON.stringify(['code']) ||
      body.scope !== 'openid profile email offline_access' ||
      JSON.stringify(body.audience) !== JSON.stringify(['gitstore']) ||
      JSON.stringify(body.redirect_uris) !== JSON.stringify(['http://127.0.0.1:3000/callback']) ||
      body.token_endpoint_auth_method !== 'client_secret_basic') return reply(400, {});
  const history = await calls();
  history.push({ method: req.method, path: req.url, clientId: body.client_id, fields: Object.keys(body).sort() });
  await pm.state.set('oidcHydra.calls', history); // Never store the secret or authorization material.
  const clients = await state();
  if (clients[body.client_id]) return reply(501, { fixture: 'Repeated creation not modeled' });
  const publicClient = { ...body };
  delete publicClient.client_secret;
  clients[body.client_id] = publicClient;
  await pm.state.set('oidcHydra.clients', clients);
  if (body.client_id.endsWith('-disconnect')) return req.socket.destroy();
  if (body.client_id.endsWith('-redirect')) {
    res.writeHead(302, { location: 'http://127.0.0.1:4446/credential-trap' });
    return res.end();
  }
  if (body.client_id.endsWith('-bad-json')) {
    res.writeHead(201, { 'content-type': 'application/json' });
    return res.end('{invalid');
  }
  if (body.client_id.endsWith('-oversized')) {
    res.writeHead(201, { 'content-type': 'application/json' });
    return res.end('x'.repeat(2 * 1024 * 1024));
  }
  const created = { ...body, registration_access_token: 'synthetic-oidc-registered-token' };
  if (body.client_id.endsWith('-wrong-client')) created.client_id = 'conflicting-client';
  if (body.client_id.endsWith('-wrong-secret')) created.client_secret = 'synthetic-mismatched-secret-value';
  if (body.client_id.endsWith('-bad-type')) {
    res.writeHead(201, { 'content-type': 'text/plain' });
    return res.end(JSON.stringify(created));
  }
  if (body.client_id.endsWith('-delay')) return setTimeout(() => reply(201, created), 2500);
  return reply(201, created);
}).listen(process.env.PORT, '127.0.0.1');
