#!/bin/sh
# Online checks: for each profile, players find each other through a Nakama server's
# matchmaker, open direct connections and play under GekkoNet with scripted keys; the
# per-frame traces of all players must agree. Every player runs on this computer. Needs a
# Nakama server with the IKEMEN GO Nakama fork's server modules (doc/netplay-client.md);
# a test server is best, since the test accounts look for matches like any player.
# Game profiles run only when their environment variables are set (doc/netplay.md).
#
# usage: tools/netplay/run_online_test.sh path/to/mgba-netplay-online-test [host [port]]
# NAKAMA_KEY sets the server key (default: defaultkey).
set -u
TOOL=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
HOST=${2:-127.0.0.1}
PORT=${3:-7350}
KEY=${NAKAMA_KEY:-defaultkey}
cd "$(dirname "$0")"
OUT=${OUT:-_test}
mkdir -p "$OUT"
status=0

have() { # profile -> 0 if its files are available
  "$TOOL" --identity --profile "profiles/$1.ini" >/dev/null 2>&1
}

online() { # name profile players frames
  name=$1 prof=$2 n=$3 frames=$4
  if ! have "$prof"; then echo "== $name: skipped (files or environment variables missing)"; return; fi
  echo "== $name: $n players matched through $HOST:$PORT, $frames frames"
  pids=""
  i=1
  while [ $i -le $n ]; do
    rm -f "$OUT/$name.online$i.trace"
    "$TOOL" --profile "profiles/$prof.ini" --host "$HOST" --port "$PORT" --key "$KEY" \
      --device "mgba-online-test-$i" --name "online-test-$i" --frames "$frames" --seed 2 \
      --trace "$OUT/$name.online$i.trace" >"$OUT/$name.online$i.out" 2>"$OUT/$name.online$i.log" &
    pids="$pids $!"
    i=$((i + 1))
  done
  for pid in $pids; do wait "$pid" || status=1; done
  grep -h "^player\|FAILED\|TIMEOUT" "$OUT/$name".online*.out
  # the last frames may have run on predicted inputs when a player stopped
  keep=$((frames - 10))
  same=1
  i=1
  while [ $i -le $n ]; do
    grep -q "desyncs 0," "$OUT/$name.online$i.out" || status=1
    if [ -f "$OUT/$name.online$i.trace" ]; then
      head -n "$keep" "$OUT/$name.online$i.trace" >"$OUT/$name.online$i.head"
    else
      : >"$OUT/$name.online$i.head"
    fi
    if [ $i -gt 1 ] && ! cmp -s "$OUT/$name.online1.head" "$OUT/$name.online$i.head"; then
      echo "   TRACES DIFFER (process 1 vs process $i)"; same=0; status=1
    fi
    i=$((i + 1))
  done
  if [ "$(wc -l <"$OUT/$name.online1.head")" -ne "$keep" ]; then
    echo "   TRACES INCOMPLETE"; status=1
  elif [ $same = 1 ]; then
    echo "   traces of all $n players identical for frames 0-$((keep - 1))"
  fi
}
online online-2p linkstress-2p 2 900
online online-vs ssf2tr-vs 2 900
online online-mk3 mksc-3p 3 600

if [ $status = 0 ]; then echo "ALL CHECKS PASSED"; else echo "CHECKS FAILED"; fi
exit $status
