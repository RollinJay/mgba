#!/bin/sh
# Netplay checks: a GekkoNet stress session (SyncTest) per profile, a negative control
# that must report desyncs, and peers over UDP on this machine with added latency, jitter
# and loss, whose per-frame traces must agree. Game profiles run only when their
# environment variables are set (see doc/netplay.md).
#
# usage: tools/netplay/run_test.sh path/to/mgba-netplay-test
set -u
TOOL=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
cd "$(dirname "$0")"
OUT=${OUT:-_test}
mkdir -p "$OUT"
status=0

have() { # profile -> 0 if its files are available
  "$TOOL" -m identity -p "profiles/$1.ini" >/dev/null 2>&1
}

for p in linkstress-2p linkstress-4p wstest-2p ssf2tr-hotseat ssf2tr-p2pad ssf2tr-vs mksc-3p mksc-4p; do
  if ! have "$p"; then echo "== $p: skipped (files or environment variables missing)"; continue; fi
  echo "== $p: SyncTest, 1500 frames"
  "$TOOL" -m synctest -p "profiles/$p.ini" -f 1500 2>"$OUT/$p.sync.log" | tee "$OUT/$p.sync.out"
  grep -q "desyncs 0," "$OUT/$p.sync.out" || status=1
done

echo "== negative control: corrupted state loads must be reported"
"$TOOL" -m synctest -p profiles/linkstress-2p.ini -f 120 -B >"$OUT/control.out" 2>"$OUT/control.log"
if grep -q "desyncs 0," "$OUT/control.out"; then
  echo "   NOT DETECTED"; status=1
else
  echo "   detected: $(grep -c DESYNC "$OUT/control.log") desync reports"
fi

peers() { # name profile players frames delay latency jitter loss
  name=$1 prof=$2 n=$3 frames=$4 delay=$5 lat=$6 jit=$7 loss=$8
  if ! have "$prof"; then echo "== $name: skipped"; return; fi
  echo "== $name: $n peers, $frames frames, input delay $delay, latency ${lat}ms + jitter up to ${jit}ms, loss $loss"
  pids=""
  i=1
  while [ $i -le $n ]; do
    list="" j=1
    while [ $j -le $n ]; do
      [ $j -ne $i ] && list="$list${list:+,}$j=127.0.0.1:$((7000 + j))"
      j=$((j + 1))
    done
    "$TOOL" -m peer -p "profiles/$prof.ini" -P $i -o $((7000 + i)) -r "$list" -f "$frames" -d "$delay" \
      -L "$lat" -J "$jit" -X "$loss" -s 2 -t "$OUT/$name.p$i.trace" >"$OUT/$name.p$i.out" 2>"$OUT/$name.p$i.log" &
    pids="$pids $!"
    i=$((i + 1))
  done
  for pid in $pids; do wait "$pid" || status=1; done
  grep -h "^player" "$OUT/$name".p*.out
  keep=$((frames - 10))
  i=1
  while [ $i -le $n ]; do
    grep -q "desyncs 0," "$OUT/$name.p$i.out" || status=1
    head -n "$keep" "$OUT/$name.p$i.trace" >"$OUT/$name.p$i.head"
    if [ $i -gt 1 ] && ! cmp -s "$OUT/$name.p1.head" "$OUT/$name.p$i.head"; then
      echo "   TRACES DIFFER (player 1 vs player $i)"; status=1
    fi
    i=$((i + 1))
  done
  echo "   traces of all $n peers identical for frames 0-$((keep - 1))"
}
peers stress-2p linkstress-2p 2 1800 1 50 30 0
peers stress-4p linkstress-4p 4 900 2 30 15 0.03
peers vs-lag ssf2tr-vs 2 1800 1 50 30 0
peers vs-loss ssf2tr-vs 2 1500 2 30 15 0.03
peers mk3-lag mksc-3p 3 1800 1 50 30 0
peers mk4-loss mksc-4p 4 900 2 30 15 0.03

if [ $status = 0 ]; then echo "ALL CHECKS PASSED"; else echo "CHECKS FAILED"; fi
exit $status
