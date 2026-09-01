#!/bin/sh
# Mint TURN credentials and load them as a Fly secret.
#
# The obvious one-liner does NOT work:
#   turn_credentials.py | sed ... | tr ... | xargs -0 -I{} flyctl secrets set ...
# `xargs` fails with "command line cannot be assembled, too long" because -0
# with no NUL in the stream makes the whole thing one enormous argument. It
# fails LOUDLY but the pipeline's exit status is easy to miss, and the result is
# a coordinator still serving the previous arm's ICE list — which is how an
# entire measurement arm gets recorded against the wrong configuration.
#
# `flyctl secrets set` reads a value from stdin with `NAME=-`, so nothing has to
# fit on a command line and the credential never enters shell history.
set -eu
APP="${1:-p2pgpu}"
TTL="${2:-7200}"

python3 tools/turn_credentials.py --ttl "$TTL" \
    | sed 's/^--ice-server //' \
    | tr '\n' ' ' \
    | flyctl secrets set -a "$APP" P2PGPU_ICE_SERVERS=-

echo "set P2PGPU_ICE_SERVERS on $APP (ttl ${TTL}s)"
echo "WAIT for the redeploy to finish before recording — a worker that connects"
echo "during it gets the OLD ice list, which is indistinguishable afterwards."
