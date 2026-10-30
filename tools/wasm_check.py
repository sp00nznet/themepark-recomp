"""Open the browser build in Chrome, let it boot, and screenshot it.

Needs the page served (tools/serve_wasm.py) and Python Playwright. Prints the
page's console; exits 1 if the page or the game thread threw. Extra arguments go to Chrome.
    python tools/wasm_check.py [url] [seconds] [out.png] [chrome flags...]
"""
import sys
from playwright.sync_api import sync_playwright

url = sys.argv[1] if len(sys.argv) > 1 else 'http://localhost:8000/themepark.html'
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 40
out = sys.argv[3] if len(sys.argv) > 3 else 'wasm_check.png'

with sync_playwright() as p:
    b = p.chromium.launch(channel='chrome', args=['--autoplay-policy=no-user-gesture-required'] + sys.argv[4:])
    pg = b.new_page(viewport={'width': 1000, 'height': 760})
    bad = []
    pg.on('console', lambda m: print('[page]', m.text))
    pg.on('pageerror', lambda e: (print('[error]', e), bad.append(str(e))))
    pg.goto(url)
    pg.wait_for_timeout(secs * 1000)
    pg.screenshot(path=out)
    print('screenshot:', out)
    b.close()
    sys.exit(1 if bad else 0)
