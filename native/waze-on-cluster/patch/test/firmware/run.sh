#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Robert Nisipeanu
# dlt_capture.js against the unit's own dlt-daemon, libdlt, dlt-receive, dlt-control and
# /etc/dlt.conf, the daemon started as dlt.service starts it (linux/amd64,
# Ubuntu 18.04 for the unit's glibc 2.27; Node 8.16.2 as on the unit).
#   test/firmware/run.sh ROOTFS
# ROOTFS is a copy of the unit's root file system (at least /usr/bin/dlt-daemon,
# /usr/bin/dlt-receive, /usr/bin/dlt-control, /usr/lib/libdlt.so.2.15.0 and
# /etc/dlt.conf).
set -eu
ROOTFS=$(cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
docker run --rm --platform linux/amd64 --network none -v "$ROOTFS:/rootfs:ro" -v "$HERE:/src:ro" -v "$W:/out" \
    node:8.16.2-stretch sh -c 'cp /usr/local/bin/node /out/node && cp /rootfs/usr/lib/libdlt.so.2.15.0 /out/libdlt.so && \
        gcc /src/client.c -o /out/client -L/out -ldlt -Wl,--allow-shlib-undefined'
docker run --rm --platform linux/amd64 --network none \
    -v "$ROOTFS:/rootfs:ro" -v "$HERE/../..:/patch:ro" -v "$W:/tools:ro" \
    ubuntu:18.04 /tools/node /patch/test/firmware/capture_test.js
