#!/usr/bin/env bash
# usage: bisect.sh step1 step2 ...   (steps: flush loadA loadB loadRA loadRB flushall flushdb list dump delete)
cd "$(dirname "$0")/../.."
CLI=/local/home/rainval/valkey-worktrees/try-valkey-wasm-native/src/valkey-cli
for p in $(pgrep -f "^node utils/try-valkey-wasm/tcp-bridge"); do kill "$p"; done
sleep 0.3
node utils/try-valkey-wasm/tcp-bridge.mjs --port 7379 > bridge.log 2>&1 &
sleep 1.5
A='"#!lua name=test\nredis.register_function('"'"'test'"'"', function(KEYS, ARGV)\n return redis.call('"'"'set'"'"', '"'"'x'"'"', '"'"'1'"'"')\nend)"'
B='"#!lua name=test\nredis.register_function('"'"'test'"'"', function(KEYS, ARGV)\n return '"'"'hello'"'"'\nend)"'
cmds=()
for s in "$@"; do
  case $s in
    flush) cmds+=("function flush");;
    loadA) cmds+=("function load $A");;
    loadB) cmds+=("function load $B");;
    loadRA) cmds+=("function load REPLACE $A");;
    loadRB) cmds+=("function load REPLACE $B");;
    flushall) cmds+=("flushall");;
    flushdb) cmds+=("flushdb");;
    list) cmds+=("function list");;
    dump) cmds+=("function dump");;
    delete) cmds+=("function delete test");;
    *) cmds+=("$s");;
  esac
done
out=$(printf '%s\n' "${cmds[@]}" | $CLI -p 7379 2>&1 | tail -1 | cut -c1-60)
sleep 0.3
printf '%-60s -> %s %s\n' "$*" "$out" "$(grep -q RuntimeError bridge.log && echo CRASH)"
