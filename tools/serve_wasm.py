"""Serve build-wasm/ at http://localhost:8000/themepark.html.

The game thread is a web worker sharing the wasm memory, which a browser
allows only on a cross-origin isolated page: these two headers.
    python3 tools/serve_wasm.py [dir] [port]
"""
import http.server
import os
import sys


class Isolated(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    os.chdir(sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', 'build-wasm'))
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
    print('http://localhost:%d/themepark.html' % port)
    http.server.ThreadingHTTPServer(('', port), Isolated).serve_forever()
