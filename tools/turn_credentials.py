#!/usr/bin/env python3
"""Mint short-lived Cloudflare TURN credentials from the key in `.env`.

    python3 tools/turn_credentials.py              # print the --ice-server args
    python3 tools/turn_credentials.py --shell      # print an export line

────────────────────────────────────────────────────────────────────────────
WHY THIS EXISTS

Cloudflare does not hand out a TURN username and password. It gives a **key ID
and an API token**, and you exchange those for credentials that expire. The API
token is a long-lived secret that can mint more; the credentials it returns are
short-lived and scoped to relaying.

That distinction is the whole reason this script exists: **the API token must
never reach a worker, and the minted credential must.** A TURN URL is handed to
every worker that connects, and every worker is untrusted — so what goes on the
wire has to be the thing that expires.

────────────────────────────────────────────────────────────────────────────
`.env` IS GITIGNORED AND MUST STAY THAT WAY

This reads it; it never echoes the token, and it prints only the minted
credential. Rotate the key from the Cloudflare dashboard if it is ever pasted
somewhere it should not be.
"""

import argparse
import json
import os
import sys
import ssl
import urllib.error
import urllib.request

def _ctx():
    # The python.org build on macOS ships no CA bundle and ignores the system
    # keychain, so HTTPS fails here while curl succeeds on the same URL.
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return None


# `credentials/generate` returns ONE iceServers object with a shared username
# and credential. The `-ice-servers` variant returns a LIST in which the stun
# entry has no credentials, and treating that shape as the first silently
# produces a URL with no username. Both work; this one has the simpler shape.
API = "https://rtc.live.cloudflare.com/v1/turn/keys/{key}/credentials/generate"


def load_env(path=".env"):
    out = {}
    try:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip().strip('"').strip("'")
    except FileNotFoundError:
        print(f"{path} not found — create it with TURN_Token_ID and "
              f"TURN_API_Token", file=sys.stderr)
        raise SystemExit(1)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--env", default=".env")
    ap.add_argument("--ttl", type=int, default=86400,
                    help="credential lifetime in seconds. Keep it SHORT: this "
                         "string is sent to every untrusted worker, so its "
                         "blast radius is exactly its lifetime.")
    ap.add_argument("--shell", action="store_true",
                    help="print an export line for P2PGPU_ICE_SERVERS")
    args = ap.parse_args()

    env = load_env(args.env)
    key = env.get("TURN_Token_ID")
    token = env.get("TURN_API_Token")
    if not key or not token:
        print("need TURN_Token_ID and TURN_API_Token in the env file",
              file=sys.stderr)
        return 1

    req = urllib.request.Request(
        API.format(key=key),
        data=json.dumps({"ttl": args.ttl}).encode(),
        headers={"Authorization": f"Bearer {token}",
                 "Content-Type": "application/json",
                 # WITHOUT THIS, CLOUDFLARE RETURNS 403 error 1010.
                 # Python's default `Python-urllib/3.x` trips the edge WAF's
                 # client-signature rule, and the response says nothing about
                 # user agents — it looks exactly like a bad API token, which
                 # is how a perfectly good key gets regenerated for no reason.
                 "User-Agent": "p2pgpu/0.1"},
        method="POST")
    try:
        with urllib.request.urlopen(req, timeout=15, context=_ctx()) as r:
            body = json.load(r)
    except urllib.error.HTTPError as e:
        # The token is NOT echoed here — an error path that prints the request
        # is how a secret ends up in a terminal scrollback and then a paste.
        print(f"Cloudflare refused: HTTP {e.code} {e.reason}", file=sys.stderr)
        print(e.read().decode(errors="replace")[:400], file=sys.stderr)
        return 1
    except urllib.error.URLError as e:
        print(f"could not reach Cloudflare: {e}", file=sys.stderr)
        return 1

    ice = body.get("iceServers") or {}
    urls = ice.get("urls") or []
    user = ice.get("username")
    cred = ice.get("credential")
    if not urls or not user or not cred:
        print(f"unexpected response shape: {list(body)}", file=sys.stderr)
        return 1

    # Only turn:/turns: entries carry credentials; a bare stun: URL does not
    # and must not be given one.
    args_out = []
    for u in urls:
        if u.startswith("stun:"):
            args_out.append(u)
        else:
            scheme, rest = u.split(":", 1)
            args_out.append(f"{scheme}:{user}:{cred}@{rest}")

    if args.shell:
        print("export P2PGPU_ICE_SERVERS='" + " ".join(args_out) + "'")
    else:
        for a in args_out:
            print(f"--ice-server {a}")
    print(f"\n# expires in {args.ttl}s. Anything holding these after that "
          f"silently loses relay.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
