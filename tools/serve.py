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
    token = ""
    results_dir = "results"

    def _browser_tag(self) -> str:
        """Short `browser-platform` tag from the User-Agent.

        Order matters for the browser: Chrome's UA contains "Safari", and
        Edge's contains both "Chrome" and "Safari", so the most specific match
        has to win. Getting this backwards would file every Chrome result under
        "safari".

        PLATFORM IS PART OF THE TAG, and that is not cosmetic. It was browser-
        only until step 0.16, when Chrome on a Windows/NVIDIA machine posted its
        throughput and **silently overwrote the Mac Chrome run** — same browser,
        completely different GPU, same filename. The whole point of that session
        was comparing two vendors, and the mechanism for collecting the data
        destroyed one side of the comparison.

        Recovered from git that time. A tag that cannot distinguish the two
        machines in a cross-vendor experiment is not a tag.
        """
        ua = self.headers.get("User-Agent", "")
        browser = "unknown"
        for needle, tag in (("Edg/", "edge"),
                            ("OPR/", "opera"),
                            ("Firefox/", "firefox"),
                            ("Chrome/", "chrome"),
                            ("Safari/", "safari")):
            if needle in ua:
                browser = tag
                break

        platform = "unknown"
        for needle, tag in (("Windows", "windows"),
                            ("Android", "android"),
                            ("iPhone", "ios"), ("iPad", "ios"),
                            ("Mac OS X", "macos"),
                            ("Linux", "linux")):
            if needle in ua:
                platform = tag
                break

        return f"{browser}-{platform}"

    @staticmethod
    def _gpu_tag(body: bytes) -> str:
        """Short GPU slug from the report body, or "" if it cannot be found.

        Both report shapes carry it: `adapter : nvidia / turing /` in the smoke
        dump, `# adapter=nvidia/turing/` in the throughput CSV.
        """
        try:
            head = body[:400].decode("utf-8", errors="replace")
        except Exception:
            return ""
        for line in head.splitlines():
            low = line.lower()
            if "adapter" not in low:
                continue
            _, _, rest = low.partition("adapter")
            rest = rest.lstrip(" :=")
            # Keep the first two fields (vendor, architecture) — enough to tell
            # a discrete card from an integrated one on the same machine.
            parts = [p.strip() for p in rest.split("/") if p.strip()]
            slug = "-".join(parts[:2])
            slug = "".join(c if c.isalnum() or c == "-" else "-" for c in slug)
            slug = "-".join(f for f in slug.split("-") if f)
            return slug[:32]
        return ""

    def _token_ok(self) -> bool:
        """Shared-secret gate. DEV ONLY, and weaker than it looks.

        Enabled only when --token is passed, so a normal loopback run is
        completely unaffected. It exists for the one case where this server is
        reachable by someone other than you — a tunnel, or --host 0.0.0.0 on a
        shared network — because `do_POST` below WRITES FILES with no
        authentication of any kind.

        GATES THE WRITE PATH ONLY, never GET. A browser loading index.html
        fetches ui.js and the .wasm through RELATIVE urls, which do not carry
        the parent page's query string — so gating GET 403s the page's own
        subresources and the app never starts. Found the hard way, mid-session.

        Gating only POST is also the right threat model: everything served here
        is public code (the coordinator serves the same WGSL to the world by
        design). The thing worth protecting is `results/`, which is where the
        measurements live and which POST can overwrite.

        HONEST LIMITS: over plain HTTP the token travels in cleartext, so it
        stops casual and accidental access, not anyone watching the wire. It is
        only meaningfully private over a tunnel that terminates TLS.
        """
        if not Handler.token:
            return True
        _, _, query = self.path.partition("?")
        for part in query.split("&"):
            key, _, value = part.partition("=")
            if key == "token" and value == Handler.token:
                return True
        return False

    def do_POST(self):  # noqa: N802 - name fixed by BaseHTTPRequestHandler
        """Capture the browser's step-0.9 report so it can be diffed offline.

        DEVELOPMENT HARNESS ONLY. Nothing in the real system posts anything to
        the coordinator this way; this exists so a cross-target comparison does
        not require copying 8000 hex characters out of a browser console by
        hand. It is unauthenticated and writes to a fixed filename, which is
        fine for a loopback-only dev server and would not be anywhere else.
        """
        if not self._token_ok():
            # 403, not 404: the endpoint's existence is not the secret, and a
            # misleading error here would send you debugging the wrong thing.
            self.send_error(403, "missing or bad ?token=")
            return

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

        # Tag the filename with the browser, so running the same step in Safari
        # does not silently overwrite the Chrome results. Derived server-side
        # from the User-Agent rather than in ui.js, which stays logic-free (R1).
        stem, ext = os.path.splitext(name)
        name = f"{stem}-{self._browser_tag()}{ext or '.txt'}"

        length = int(self.headers.get("Content-Length", 0))

        # An empty body is NOT "too large". Lumping the two together produced a
        # baffling `413 Content Too Large` when Safari failed before generating
        # a report — a misleading error on top of the real one, which is the
        # last thing you want while diagnosing a failure.
        if length <= 0:
            self.send_error(400, "empty report (did the run fail before producing one?)")
            return

        # Bound it. Even a dev tool should not let a stray request allocate
        # arbitrary memory — the report is a few KB.
        if length > 4 * 1024 * 1024:
            self.send_error(413)
            return

        body = self.rfile.read(length)

        # ── THE GPU GOES IN THE FILENAME TOO ─────────────────────────────
        # browser+platform is not unique enough. A machine with both a discrete
        # and an integrated GPU reports the SAME `chrome-windows` for each, so
        # the second run silently overwrites the first — and that machine is
        # exactly the case worth capturing, because it is a third distinct GPU
        # for free.
        #
        # Same failure as the one that clobbered the Mac Chrome run in 0.16,
        # one level down: that fix added the platform, and the platform is not
        # the last thing that distinguishes two runs.
        #
        # Read from the report BODY rather than asked of ui.js, which stays
        # logic-free (R1) — the adapter line is already in every report.
        gpu = self._gpu_tag(body)
        if gpu:
            stem2, ext2 = os.path.splitext(name)
            name = f"{stem2}-{gpu}{ext2}"
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
    parser.add_argument("--host", default="127.0.0.1",
                        help="bind address. Default is loopback only. Use "
                             "--host 0.0.0.0 to let ANOTHER MACHINE on your "
                             "network reach it (step 0.16).")
    parser.add_argument("--token", default="",
                        help="require ?token=... on every request. OFF by "
                             "default, so a normal localhost run is unchanged. "
                             "Use it whenever this server is reachable by "
                             "anyone else (a tunnel, or --host 0.0.0.0).")
    args = parser.parse_args()

    Handler.results_dir = os.path.abspath(args.results)
    Handler.token = args.token

    if not os.path.isdir(args.dir):
        print(f"error: {args.dir} does not exist — run `cmake --build build/wasm` first")
        return 1

    handler = functools.partial(Handler, directory=args.dir)
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer((args.host, args.port), handler) as httpd:
        suffix = f"/?token={args.token}" if args.token else ""
        print(f"serving {args.dir} at http://{args.host}:{args.port}{suffix}")
        print("  COOP/COEP enabled (cross-origin isolated)")
        if args.token:
            # Printed complete so it can be copied rather than assembled by
            # hand — a token typo presents as a blank page, which is a
            # miserable thing to debug on someone else's machine.
            print(f"  token REQUIRED — open exactly: "
                  f"http://{args.host}:{args.port}/?token={args.token}")
        if args.host not in ("127.0.0.1", "localhost"):
            # Say this plainly. The POST handler writes files and does no
            # authentication whatsoever — fine on loopback, a real exposure on
            # a shared or public network.
            print("  WARNING: reachable from the network. This server accepts "
                  "unauthenticated POSTs that WRITE FILES.")
            print("           Use only on a trusted network, and stop it when done.")
        print("  Ctrl-C to stop")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
