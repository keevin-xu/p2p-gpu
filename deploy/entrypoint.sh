#!/bin/sh
# Turn $P2PGPU_ICE_SERVERS into repeated --ice-server flags.
#
# WHY NOT BAKE THEM INTO THE IMAGE
# A TURN credential from Cloudflare EXPIRES. Baked into a layer it would sit in
# the image after it stopped working, and the deployment would keep serving a
# dead credential to every worker — relay silently unavailable, with a config
# that still looks correct. Passing it as a secret also keeps it out of the
# image's layers, which anyone who can pull the image can read.
#
# `exec` IS LOAD-BEARING. It replaces this shell with the coordinator so the
# coordinator stays PID 1 and receives the platform's SIGTERM directly. Without
# it, the shell holds PID 1, ignores SIGTERM, and every stop escalates to
# SIGKILL — which is exactly the bug that cost an afternoon here already.
set -eu

for url in ${P2PGPU_ICE_SERVERS:-}; do
    set -- "$@" --ice-server "$url"
done

exec /app/coordinator "$@"
