"""Local TLS front end for the synthetic Qt HTTP fixture (standard library only)."""
import argparse
import http.client
import http.server
import ssl


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--upstream', type=int, required=True)
    parser.add_argument('--certificate', required=True)
    parser.add_argument('--key', required=True)
    args = parser.parse_args()

    class Handler(http.server.BaseHTTPRequestHandler):
        def forward(self):
            body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            upstream = http.client.HTTPConnection('127.0.0.1', args.upstream, timeout=5)
            try:
                upstream.request(self.command, self.path, body, dict(self.headers))
                response = upstream.getresponse()
                data = response.read()
                self.send_response(response.status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.send_header('Connection', 'close')
                self.end_headers()
                self.wfile.write(data)
            finally:
                upstream.close()

        do_GET = do_POST = do_DELETE = do_PATCH = forward

        def log_message(self, *_args):
            pass

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(args.certificate, args.key)
    with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        server.socket = context.wrap_socket(server.socket, server_side=True)
        print(server.server_port, flush=True)
        server.serve_forever()


if __name__ == '__main__':
    main()
