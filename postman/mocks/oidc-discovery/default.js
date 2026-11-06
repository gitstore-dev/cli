// Controlled public-document envelopes; the synthetic modulus is not token evidence.
const http = require('http');
http.createServer((req, res) => {
  const base = `http://127.0.0.1:${process.env.PORT}`;
  const routes = {
    '/.well-known/openid-configuration': { issuer: `${base}/`, jwks_uri: `${base}/.well-known/jwks.json` },
    '/.well-known/jwks.json': { keys: [{ kty: 'RSA', kid: 'synthetic-public-key', use: 'sig',
      alg: 'RS256', n: 'x'.repeat(342), e: 'AQAB' }] },
  };
  const value = req.method === 'GET' && routes[req.url];
  res.writeHead(value ? 200 : 501, { 'content-type': 'application/json' });
  res.end(JSON.stringify(value || { fixture: 'Outside modeled consumer contract' }));
}).listen(process.env.PORT, '127.0.0.1');
