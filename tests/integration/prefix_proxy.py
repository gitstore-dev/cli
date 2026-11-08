"""Disposable loopback path-prefix proxy, including WebSocket upgrade tunnelling.

Test infrastructure only; bounded headers/body and socket timeouts. Never logs
credentials, never listens publicly, never implements an application API.
"""
import http.client
import http.server
import select
import socket
import threading
import urllib.parse


class Proxy:
    def __init__(self, endpoint):
        target = urllib.parse.urlsplit(endpoint)
        if target.scheme != 'http' or target.hostname != '127.0.0.1':
            raise ValueError('proxy fixture requires the owned loopback upstream')
        host, port = target.hostname, target.port

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def route(self):
                if self.path != '/gitstore/admin/graphql':
                    self.send_error(404)
                    return None
                return '/admin/graphql'

            def do_POST(self):
                self.server.request_count += 1
                path = self.route()
                if path is None:
                    return
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 16 * 1024 * 1024:
                    self.send_error(413)
                    return
                self.connection.settimeout(10)
                body = self.rfile.read(length)
                headers = {name: value for name, value in self.headers.items()
                           if name.lower() not in ('host', 'connection', 'accept-encoding')}
                headers['Host'] = self.headers['Host']
                headers['Connection'] = 'close'
                upstream = http.client.HTTPConnection(host, port, timeout=15)
                try:
                    upstream.request('POST', path, body, headers)
                    response = upstream.getresponse()
                    payload = response.read(16 * 1024 * 1024 + 1)
                    if len(payload) > 16 * 1024 * 1024:
                        raise ValueError('proxy response too large')
                    self.send_response(response.status)
                    self.send_header('Content-Type', response.getheader('Content-Type', 'application/json'))
                    self.send_header('Content-Length', str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                finally:
                    upstream.close()

            def do_GET(self):
                path = self.route()
                if path is None:
                    return
                if self.headers.get('Upgrade', '').lower() != 'websocket':
                    self.send_error(400)
                    return
                self.connection.settimeout(5)
                with socket.create_connection((host, port), timeout=5) as upstream:
                    # Preserve the public Host for the upstream's same-origin
                    # WebSocket check. Normalize case to avoid duplicate Host.
                    headers = {name: value for name, value in self.headers.items() if name.lower() != 'host'}
                    headers['Host'] = self.headers['Host']
                    request = 'GET ' + path + ' HTTP/1.1\r\n' + ''.join(
                        name + ': ' + value + '\r\n' for name, value in headers.items()) + '\r\n'
                    upstream.sendall(request.encode('latin-1'))
                    response = bytearray()
                    while not response.endswith(b'\r\n\r\n'):
                        chunk = upstream.recv(1)
                        if not chunk or len(response) >= 65536:
                            raise ValueError('proxy upgrade headers invalid')
                        response.extend(chunk)
                    self.connection.sendall(response)
                    if not response.startswith(b'HTTP/1.1 101 '):
                        return
                    peers = (self.connection, upstream)
                    while True:
                        readable, _, _ = select.select(peers, [], [], 25)
                        if not readable:
                            return
                        for source in readable:
                            data = source.recv(65536)
                            if not data:
                                return
                            (upstream if source is self.connection else self.connection).sendall(data)

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.request_count = 0
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.endpoint = f'http://127.0.0.1:{self.server.server_port}/gitstore/admin/graphql'

    def start(self):
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
