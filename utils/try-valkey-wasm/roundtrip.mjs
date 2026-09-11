// Round-trip probe for the in-process wasm server.
// Usage: node roundtrip.mjs  (prints raw RESP replies for a fixed script)
import createValkey from '../../src/valkey-server.mjs';

const quiet = process.env.TV_QUIET === '1';
const Module = await createValkey({
  print: (s) => { if (!quiet) console.log('[srv]', s); },
  printErr: (s) => console.log('[srv-err]', s),
  noInitialRun: true,
});

const SERVER_ARGS = [
  '--port', '0', '--save', '', '--appendonly', 'no', '--maxclients', '64',
  '--maxmemory', '64mb', '--maxmemory-policy', 'allkeys-lru', '--loglevel', 'notice',
];

// main() "returns" via emscripten_exit_with_live_runtime inside aeMain.
try { Module.callMain(SERVER_ARGS); } catch (e) { if (e !== 'unwind' && e?.name !== 'ExitStatus') throw e; }

// Pointers cross the boundary as BigInt in a wasm64-compiled module.
const P = (ptr) => BigInt(ptr);
const tv_connect = () => Module._tv_connect();
const tv_write = (id, ptr, len) => Module._tv_write(id, P(ptr), len);
const tv_tick = () => Module._tv_tick();
const tv_pending = (id) => Module._tv_pending(id);
const tv_read = (id, ptr, cap) => Module._tv_read(id, P(ptr), cap);
const tv_close = (id) => Module._tv_close(id);

const BUF_CAP = 1 << 16;
const bufPtr = Module._malloc(BUF_CAP);

function encodeRESP(args) {
  let s = `*${args.length}\r\n`;
  for (const a of args) {
    const b = Buffer.from(String(a));
    s += `$${b.length}\r\n${b}\r\n`;
  }
  return Buffer.from(s);
}

function send(id, bytes) {
  Module.HEAPU8.set(bytes, Number(bufPtr));
  const rc = tv_write(id, bufPtr, bytes.length);
  if (rc < 0) throw new Error(`tv_write failed on conn ${id}`);
  tv_tick();
}

function drain(id) {
  let out = '';
  for (;;) {
    const n = tv_read(id, bufPtr, BUF_CAP);
    if (n <= 0) break;
    out += Buffer.from(Module.HEAPU8.buffer, Number(bufPtr), n).toString('latin1');
    if (n < BUF_CAP) break;
  }
  return out;
}

function cmd(id, ...args) {
  send(id, encodeRESP(args));
  return drain(id);
}

const show = (s) => JSON.stringify(s);

const c1 = tv_connect();
console.log('conn id', c1);

const script = [
  ['PING'],
  ['SET', 'k', 'v'],
  ['GET', 'k'],
  ['INCR', 'n'], ['INCRBYFLOAT', 'n', '1.5'],
  ['MULTI'], ['SET', 'a', '1'], ['INCR', 'a'], ['EXEC'],
  ['HSET', 'h', 'f1', 'x', 'f2', 'y'], ['HGETALL', 'h'],
  ['LPUSH', 'l', 'c', 'b', 'a'], ['LRANGE', 'l', '0', '-1'],
  ['ZADD', 'z', '1', 'one', '2', 'two'], ['ZRANGE', 'z', '0', '-1', 'WITHSCORES'],
  ['SET', 'ttl', 'x', 'PX', '50'], ['TTL', 'ttl'],
  ['EVAL', "return {KEYS[1], ARGV[1], redis.call('GET', 'k')}", '1', 'k', 'arg'],
  ['GET'],                       // arity error text from the real server
  ['LPUSH', 'k', 'x'],           // WRONGTYPE
  ['HELLO', '3'], ['HGETALL', 'h'], ['ZRANGE', 'z', '0', '-1', 'WITHSCORES'],
  ['CLIENT', 'LIST'],
  ['INFO', 'server'],
  ['DBSIZE'],
];

for (const args of script) {
  const reply = cmd(c1, ...args);
  console.log(`> ${args.join(' ')}\n${show(reply)}`);
}

// Second connection: pub/sub across two mem connections.
const c2 = tv_connect();
console.log('> [c2] SUBSCRIBE chan\n' + show(cmd(c2, 'SUBSCRIBE', 'chan')));
console.log('> [c1] PUBLISH chan hello\n' + show(cmd(c1, 'PUBLISH', 'chan', 'hello')));
console.log('< [c2] pushed:\n' + show(drain(c2)));

// Expiry via serverCron: wait past the 50ms TTL, tick, then read.
await new Promise((r) => setTimeout(r, 120));
tv_tick();
console.log('> GET ttl (after 120ms)\n' + show(cmd(c1, 'GET', 'ttl')));

// Blocking pop with timeout, served by cron.
send(c1, encodeRESP(['BLPOP', 'empty', '0.1']));
console.log('> BLPOP empty 0.1 (immediate)\n' + show(drain(c1)));
await new Promise((r) => setTimeout(r, 150));
tv_tick();
console.log('< BLPOP after 150ms\n' + show(drain(c1)));

tv_close(c2);
tv_tick();
console.log('> CLIENT LIST after c2 close\n' + show(cmd(c1, 'CLIENT', 'LIST')));
console.log('done');
