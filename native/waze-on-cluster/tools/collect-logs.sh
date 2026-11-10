#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Robert Nisipeanu
# collect-logs.sh — gather what a report of the map on the cluster needs into
# one archive, to send back. Run it on the head unit as root:
#
#   sh /var/opt/waze-on-cluster/current/collect-logs.sh [-o <folder>]
#
# It writes woc-logs-<UTC date and time>.tar.gz into the folder (/tmp by
# default) and prints its path: both tools' log files (with their .1), the
# journal lines the journal service keeps (journal.log and its rotations),
# the renderer's and the capture's status files, this boot's journal of the
# tools and of carplay (what the journal still holds), the list of boots,
# the carplay patch probe (where its edit sites are in this carplay), the
# tools' folders, their local settings, the time and the uptime,
# wifi.txt: CarPlay's Wi-Fi band as patchd last read it, each P2P
# interface's /proc/net/dev counters and the kernel's last 802.1X and DHCP
# lines for them, the aapd patch's status (aapd_patch.js status: its build,
# edits and backups, and the cluster coding), and what logs_extra.js reads:
# the patch records, the aapd boot recovery's state, the restart records the
# boot step kept, the cluster coding's
# KI_KOMBI_VARIANT, the deploy tool's versions and state, and BMW's restart
# records, recmgr's crash table and the list of kept core dumps (root's to
# read: the logs service gets them from the snapshot its firewall unit takes
# as root, in SNAPSHOT; run by root, it takes them itself). Anything missing
# is left out. It changes nothing on the unit but the archive.
# POSIX sh, for the unit's shell.
set -u
umask 077

PROC_NET_DEV=${WOC_PROC_NET_DEV:-/proc/net/dev}
WIFI_BAND=${WOC_WIFI_FILE:-/dev/shm/waze-on-cluster/wifi-band.json}
SNAPSHOT=${WOC_LOGS_SNAPSHOT:-/run/waze-on-cluster-logs}

OUTDIR=/tmp
if [ "${1:-}" = -o ] && [ -n "${2:-}" ]; then
    OUTDIR=$2
elif [ $# -gt 0 ]; then
    echo "usage: collect-logs.sh [-o <folder>]" >&2
    exit 2
fi

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
NAME=woc-logs-$STAMP
OUT=$OUTDIR/$NAME.tar.gz
PART=$OUTDIR/.$NAME.tar.gz.part
STAGE=$(mktemp -d /tmp/woc-logs.XXXXXX) || { echo "collect-logs: cannot make a folder in /tmp" >&2; exit 1; }
trap 'rm -rf "$STAGE"; rm -f "$PART"' EXIT
trap 'exit 1' INT TERM
DIR=$STAGE/$NAME
mkdir -p "$DIR"

# copy SRC DEST_NAME: a regular file (never a link), silently skipped otherwise
copy() {
    if [ -f "$1" ] && [ ! -L "$1" ]; then
        cat "$1" > "$DIR/$2" 2>/dev/null || rm -f "$DIR/$2"
    fi
}

for tool in cluster-render waze-on-cluster; do
    for f in "$tool.log" "$tool.log.1"; do
        copy "/var/opt/$tool/log/$f" "$f"
    done
    copy "/var/opt/$tool/$tool.local.conf" "$tool.local.conf"
done
for f in /var/opt/waze-on-cluster/journal/journal.log*; do
    copy "$f" "kept-${f##*/}"
done
copy /var/opt/waze-on-cluster/boot-recovery.json boot-recovery.json
copy /var/opt/waze-on-cluster/shutdown boot-recovery-shutdown

for f in /run/cluster-render/*; do
    copy "$f" "run-cluster-render-${f##*/}"
done
for f in status.json probe.txt; do
    copy "/run/waze-on-cluster/$f" "run-waze-on-cluster-$f"
done

if command -v journalctl >/dev/null 2>&1; then
    for unit in cluster-render waze-on-cluster waze-on-cluster-patchd waze-on-cluster-boot waze-on-cluster-recovery \
            waze-on-cluster-capture waze-on-cluster-journal waze-on-cluster-journal.timer waze-on-cluster-logs \
            waze-on-cluster-logs-fw carplay; do
        journalctl -b -o short-precise --no-pager -u "$unit" > "$DIR/journal-$unit.txt" 2>/dev/null
    done
    journalctl --list-boots --no-pager > "$DIR/boots.txt" 2>/dev/null
fi

HERE=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
NODE=$(command -v node || echo /usr/bin/node)
if [ -f "$HERE/carplay_patch.js" ]; then
    "$NODE" "$HERE/carplay_patch.js" probe > "$DIR/carplay-probe.txt" 2>&1
fi
if [ -f "$HERE/aapd_patch.js" ]; then
    "$NODE" "$HERE/aapd_patch.js" status > "$DIR/aapd-status.txt" 2>&1
fi
if [ -f "$HERE/logs_extra.js" ]; then
    "$NODE" "$HERE/logs_extra.js" report "$DIR" > /dev/null 2>&1
    if [ "$(id -u 2>/dev/null)" = 0 ]; then
        "$NODE" "$HERE/logs_extra.js" snapshot "$DIR" > /dev/null 2>&1
    else
        for f in "$SNAPSHOT"/*; do
            copy "$f" "${f##*/}"
        done
    fi
fi

ls -l /var/opt/cluster-render /var/opt/waze-on-cluster /var/opt/waze-on-cluster/journal /var/opt/waze-on-cluster/saved > "$DIR/ls.txt" 2>&1

# The kernel's lines for the P2P interfaces: the journal's (the logs
# service's group reads it), else dmesg's.
P2P_LINES='(802_1X|DHCP)\[p2p-'
{
    echo "-- CarPlay Wi-Fi band (patchd, $WIFI_BAND)"
    cat "$WIFI_BAND" 2>/dev/null || echo "(none)"
    echo "-- P2P interfaces in $PROC_NET_DEV"
    grep -E '^ *p2p-[^:]*:' "$PROC_NET_DEV" 2>/dev/null || echo "(none)"
    echo "-- 802.1X and DHCP lines for P2P interfaces (the last 60)"
    LINES=$(journalctl -k -b -o short-precise --no-pager 2>/dev/null | grep -E "$P2P_LINES" | tail -n 60)
    [ -n "$LINES" ] || LINES=$(dmesg 2>/dev/null | grep -E "$P2P_LINES" | tail -n 60)
    if [ -n "$LINES" ]; then echo "$LINES"; else echo "(none)"; fi
} > "$DIR/wifi.txt"

{
    echo "date: $(date 2>/dev/null)"
    echo "utc: $(date -u 2>/dev/null)"
    echo "uptime: $(uptime 2>/dev/null)"
} > "$DIR/time.txt"

if ! tar -cf "$STAGE/$NAME.tar" -C "$STAGE" "$NAME" 2>/dev/null || ! gzip -c "$STAGE/$NAME.tar" > "$PART" ||
        ! mv "$PART" "$OUT"; then
    echo "collect-logs: could not write $OUT" >&2
    exit 1
fi
echo "$OUT"
