#!/usr/bin/env python3
"""Serve the built browser worker with cross-origin isolation headers.

    python3 tools/serve.py [--dir build/wasm/web] [--port 8000]

Why this exists rather than `python3 -m http.server`:

COOP/COEP are required for SharedArrayBuffer, which is required for pthreads,
which step 1.24 needs to move the task loop off the browser's main thread. The
plain http.server module sets neither. docs/RISKS.md §1 is explicit that these
headers must be set from the first deploy, not bolted on at the end — a worker
that only ever ran without cross-origin isolation will break the moment it is
turned on, and by then every earlier test result is suspect.

Step 0.6 does not strictly need them (no threads yet). They are on anyway so
the environment never silently differs from production.

Also disables caching, because a stale .wasm after a rebuild is a genuinely
confusing way to lose twenty minutes.
"""

import argparse
import functools
import http.server
import os
import socketserver


class Handler(http.server.SimpleHTTPRequestHandler):
    # Where POSTed reports land. Set from main() so it does not depend on the
    # served directory.
    results_dir = "results"

    def do_POST(self):  # noqa: N802 - name fixed by BaseHTTPRequestHandler
        """Capture the browser's step-0.9 report so it can be diffed offline.

        DEVELOPMENT HARNESS ONLY. Nothing in the real system posts anything to
        the coordinator this way; this exists so a cross-target comparison does
        not require copying 8000 hex characters out of a browser console by
        hand. It is unauthenticated and writes to a fixed filename, which is
        fine for a loopback-only dev server and would not be anywhere else.
        """
        path, _, query = self.path.partition("?")
        if path != "/report":
            self.send_error(404)
            return

        # Optional ?name=... so different steps land in different files.
        # Sanitized to a bare filename: a dev tool still must not let a request
        # choose where on disk to write.
        name = "0.9-browser.txt"
        for part in query.split("&"):
            key, _, value = part.partition("=")
            if key == "name" and value:
                candidate = os.path.basename(value)
                if candidate and candidate not in (".", "..") and "/" not in candidate:
                    name = candidate

        length = int(self.headers.get("Content-Length", 0))
        # Bound it. Even a dev tool should not let a stray request allocate
        # arbitrary memory — the report is a few KB.
        if length <= 0 or length > 4 * 1024 * 1024:
            self.send_error(413)
            return

        body = self.rfile.read(length)
        os.makedirs(self.results_dir, exist_ok=True)
        out_path = os.path.join(self.results_dir, name)
        with open(out_path, "wb") as f:
            f.write(body)
        print(f"  wrote {out_path} ({len(body)} bytes)")

        self.send_response(204)
        self.end_headers()

    def end_headers(self):
        # Cross-origin isolation (docs/RISKS.md §1).
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        # Never serve a stale wasm after a rebuild.
        self.send_header("Cache-Control", "no-store, must-revalidate")
        super().end_headers()

    def log_message(self, fmt, *args):
        print(f"  {self.address_string()} - {fmt % args}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", default="build/wasm/web",
                        help="directory to serve (default: build/wasm/web)")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--results", default="results",
                        help="where POSTed reports land (default: results)")
    args = parser.parse_args()

    Handler.results_dir = os.path.abspath(args.results)

    if not os.path.isdir(args.dir):
        print(f"error: {args.dir} does not exist — run `cmake --build build/wasm` first")
        return 1

    handler = functools.partial(Handler, directory=args.dir)
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", args.port), handler) as httpd:
        print(f"serving {args.dir} at http://localhost:{args.port}")
        print("  COOP/COEP enabled (cross-origin isolated)")
        print("  Ctrl-C to stop")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
