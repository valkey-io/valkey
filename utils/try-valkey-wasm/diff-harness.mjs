// Differential test: replay one deterministic command stream against
//   (a) the wasm server through the in-process mem connection, and
//   (b) a native valkey-server at the same SHA over TCP,
// and compare the raw RESP bytes of every reply.
//
// Usage: node diff-harness.mjs <native-valkey-server-binary>
import net from 'node:net';
import { spawn } from 'node:child_process';
import createValkey from '../../src/valkey-server.mjs';

const NATIVE_BIN = process.argv[2];
if (!NATIVE_BIN) { console.error('usage: diff-harness.mjs <native valkey-server>'); process.exit(2); }

const COMMON_ARGS = ['--save', '', '--appendonly', 'no', '--maxclients', '64', '--maxmemory', '64mb',
  '--maxmemory-policy', 'allkeys-lru', '--loglevel', 'warning'];

// ---------- RESP framing (enough to find reply boundaries, RESP2+3) ----------
function frameLength(buf, start = 0) {
  // returns byte length of one complete RESP value at buf[start], or -1 if incomplete
  if (start >= buf.length) return -1;
  const t = String.fromCharCode(buf[start]);
  const eol = buf.indexOf('\r\n', start);
  if (eol < 0) return -1;
  const line = buf.toString('latin1', start + 1, eol);
  const hdr = eol + 2 - start;
  switch (t) {
    case '+': case '-': case ':': case '_': case ',': case '#': case '(':
      return hdr;
    case '$': case '!': case '=': {
      const n = parseInt(line, 10);
      if (n < 0) return hdr;
      const need = hdr + n + 2;
      return start + need <= buf.length ? need : -1;
    }
    case '*': case '~': case '>': {
      const n = parseInt(line, 10);
      if (n < 0) return hdr;
      let off = hdr;
      for (let i = 0; i < n; i++) { const l = frameLength(buf, start + off); if (l < 0) return -1; off += l; }
      return off;
    }
    case '%': case '|': {
      const n = parseInt(line, 10);
      let off = hdr;
      for (let i = 0; i < 2 * n; i++) { const l = frameLength(buf, start + off); if (l < 0) return -1; off += l; }
      if (t === '|') { const l = frameLength(buf, start + off); if (l < 0) return -1; off += l; } // attribute + value
      return off;
    }
    default:
      throw new Error(`bad RESP type byte ${JSON.stringify(t)} at ${start}`);
  }
}

function encodeRESP(args) {
  let s = `*${args.length}\r\n`;
  for (const a of args) { const b = Buffer.from(String(a)); s += `$${b.length}\r\n${b}\r\n`; }
  return Buffer.from(s);
}

// ---------- wasm side ----------
async function startWasm() {
  const Module = await createValkey({ print: () => {}, printErr: (s) => console.error('[wasm]', s), noInitialRun: true });
  try { Module.callMain(['--port', '0', ...COMMON_ARGS]); } catch (e) { if (e !== 'unwind' && e?.name !== 'ExitStatus') throw e; }
  const CAP = 1 << 20;
  const ptr = Module._malloc(CAP);
  const id = Module._tv_connect();
  let pending = Buffer.alloc(0);
  return {
    name: 'wasm',
    async call(args) {
      const bytes = encodeRESP(args);
      Module.HEAPU8.set(bytes, Number(ptr));
      Module._tv_write(id, BigInt(ptr), bytes.length);
      for (let i = 0; i < 200; i++) {
        Module._tv_tick();
        const n = Module._tv_read(id, BigInt(ptr), CAP);
        if (n > 0) pending = Buffer.concat([pending, Buffer.from(Module.HEAPU8.buffer, Number(ptr), n)]);
        const l = frameLength(pending);
        if (l >= 0) { const out = pending.subarray(0, l); pending = pending.subarray(l); return out; }
        await new Promise((r) => setTimeout(r, 5));
      }
      throw new Error(`wasm: no complete reply for ${args.join(' ')}`);
    },
    close() {},
  };
}

// ---------- native side ----------
async function startNative() {
  const port = 20000 + Math.floor(Math.random() * 20000);
  const proc = spawn(NATIVE_BIN, ['--port', String(port), '--bind', '127.0.0.1', ...COMMON_ARGS], { stdio: ['ignore', 'ignore', 'inherit'] });
  const sock = await new Promise((resolve, reject) => {
    let tries = 0;
    const attempt = () => {
      const s = net.connect(port, '127.0.0.1');
      s.once('connect', () => resolve(s));
      s.once('error', () => { if (++tries > 100) reject(new Error('native server did not come up')); else setTimeout(attempt, 20); });
    };
    attempt();
  });
  let pending = Buffer.alloc(0);
  const waiters = [];
  sock.on('data', (d) => { pending = Buffer.concat([pending, d]); pump(); });
  function pump() {
    while (waiters.length) {
      const l = frameLength(pending);
      if (l < 0) return;
      const out = pending.subarray(0, l); pending = pending.subarray(l);
      waiters.shift()(out);
    }
  }
  return {
    name: 'native',
    call(args) { return new Promise((resolve) => { waiters.push(resolve); sock.write(encodeRESP(args)); pump(); }); },
    close() { sock.destroy(); proc.kill('SIGKILL'); },
  };
}

// ---------- corpus: hand-written cases + seeded random ops ----------
function mulberry32(a) { return () => { a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

function buildCorpus() {
  const c = [];
  const add = (...a) => c.push(a);
  // basics + error text
  add('PING'); add('PING', 'hi'); add('ECHO', 'x'); add('GET'); add('NOSUCHCOMMAND', 'a'); add('SET', 'k'); add('SET', 'k', 'v', 'BOGUS');
  add('SET', 'k', 'v'); add('GET', 'k'); add('APPEND', 'k', 'w'); add('STRLEN', 'k'); add('GETRANGE', 'k', '0', '0'); add('SETRANGE', 'k', '1', 'Z');
  add('INCR', 'n'); add('INCRBY', 'n', '41'); add('DECRBY', 'n', '2'); add('INCRBYFLOAT', 'n', '0.5'); add('INCR', 'k');
  add('SET', 'big', '9223372036854775807'); add('INCR', 'big'); add('SET', 'neg', '-9223372036854775808'); add('DECR', 'neg');
  add('SETNX', 'k', 'z'); add('SETNX', 'k2', 'z'); add('MSET', 'm1', 'a', 'm2', 'b'); add('MGET', 'm1', 'm2', 'nope'); add('MSETNX', 'm1', 'x', 'm9', 'y');
  add('GETDEL', 'k2'); add('GETEX', 'm1', 'PERSIST'); add('SET', 'ex', 'v', 'EX', '1000'); add('TTL', 'ex'); add('PERSIST', 'ex'); add('TTL', 'ex'); add('TTL', 'nokey');
  add('EXPIRE', 'm1', '100', 'NX'); add('EXPIRE', 'm1', '100', 'NX'); add('EXPIRE', 'm1', '100', 'GT'); add('EXPIRE', 'm1', '200', 'GT');
  add('TYPE', 'k'); add('TYPE', 'nokey'); add('EXISTS', 'k', 'nokey', 'm1'); add('RENAME', 'k', 'kk'); add('RENAME', 'nokey', 'x'); add('RENAMENX', 'kk', 'm1');
  add('COPY', 'kk', 'kcopy'); add('OBJECT', 'ENCODING', 'kk'); add('OBJECT', 'ENCODING', 'n'); add('OBJECT', 'ENCODING', 'nokey');
  add('SETBIT', 'bits', '7', '1'); add('GETBIT', 'bits', '7'); add('BITCOUNT', 'bits'); add('BITPOS', 'bits', '1'); add('BITOP', 'NOT', 'bits2', 'bits'); add('GET', 'bits2');
  // hashes
  add('HSET', 'h', 'a', '1', 'b', '2', 'c', '3'); add('HGET', 'h', 'a'); add('HGET', 'h', 'zz'); add('HGETALL', 'h'); add('HINCRBY', 'h', 'a', '5'); add('HINCRBYFLOAT', 'h', 'b', '1.25');
  add('HLEN', 'h'); add('HKEYS', 'h'); add('HVALS', 'h'); add('HEXISTS', 'h', 'c'); add('HDEL', 'h', 'c', 'zz'); add('HSETNX', 'h', 'a', '9'); add('HSTRLEN', 'h', 'b'); add('HMGET', 'h', 'a', 'b', 'zz');
  add('HGET', 'kk', 'a'); add('HSET', 'h', 'odd'); add('OBJECT', 'ENCODING', 'h');
  // lists
  add('RPUSH', 'l', '1', '2', '3', '4', '5'); add('LPUSH', 'l', '0'); add('LRANGE', 'l', '0', '-1'); add('LINDEX', 'l', '2'); add('LINDEX', 'l', '99'); add('LLEN', 'l');
  add('LPOP', 'l'); add('RPOP', 'l', '2'); add('LPOP', 'l', '0'); add('LSET', 'l', '0', 'x'); add('LSET', 'l', '99', 'x'); add('LINSERT', 'l', 'BEFORE', 'x', 'w'); add('LPOS', 'l', '3');
  add('LREM', 'l', '0', '3'); add('LTRIM', 'l', '0', '1'); add('LRANGE', 'l', '0', '-1'); add('LMOVE', 'l', 'l2', 'LEFT', 'RIGHT'); add('LRANGE', 'l2', '0', '-1'); add('RPOPLPUSH', 'l', 'l2');
  add('LPUSHX', 'nokey', 'v'); add('LPOP', 'nokey'); add('BLPOP', 'nokey', '0.001'); add('OBJECT', 'ENCODING', 'l2');
  // sets (kept small so listpack encoding preserves insertion order)
  add('SADD', 's', 'a', 'b', 'c', 'a'); add('SMEMBERS', 's'); add('SCARD', 's'); add('SISMEMBER', 's', 'a'); add('SMISMEMBER', 's', 'a', 'z');
  add('SADD', 's2', 'b', 'c', 'd'); add('SINTER', 's', 's2'); add('SUNION', 's', 's2'); add('SDIFF', 's', 's2'); add('SINTERCARD', '2', 's', 's2'); add('SREM', 's', 'a', 'z'); add('SMOVE', 's', 's2', 'b');
  add('SADD', 'ints', '3', '1', '2'); add('SMEMBERS', 'ints'); add('OBJECT', 'ENCODING', 'ints'); add('OBJECT', 'ENCODING', 's');
  // zsets
  add('ZADD', 'z', '1', 'a', '2', 'b', '3', 'c'); add('ZADD', 'z', 'NX', '5', 'a'); add('ZADD', 'z', 'XX', 'CH', '5', 'a'); add('ZADD', 'z', 'INCR', '1', 'b'); add('ZADD', 'z', 'nan', 'q');
  add('ZRANGE', 'z', '0', '-1', 'WITHSCORES'); add('ZRANGE', 'z', '(1', '+inf', 'BYSCORE', 'WITHSCORES'); add('ZREVRANGE', 'z', '0', '1'); add('ZRANK', 'z', 'c'); add('ZREVRANK', 'z', 'c', 'WITHSCORE');
  add('ZSCORE', 'z', 'a'); add('ZSCORE', 'z', 'nope'); add('ZCARD', 'z'); add('ZCOUNT', 'z', '-inf', '+inf'); add('ZINCRBY', 'z', '1.5', 'c'); add('ZPOPMIN', 'z'); add('ZPOPMAX', 'z', '5'); add('ZRANGE', 'z', '0', '-1');
  add('ZADD', 'z2', '1', 'x', '2', 'y'); add('ZUNIONSTORE', 'zu', '2', 'z', 'z2', 'WEIGHTS', '2', '3'); add('ZRANGE', 'zu', '0', '-1', 'WITHSCORES'); add('ZMSCORE', 'z2', 'x', 'nope');
  add('ZRANGEBYLEX', 'z2', '-', '+'); add('ZADD', 'z2', '1e400', 'inf'); add('OBJECT', 'ENCODING', 'z2');
  // transactions + watch
  add('MULTI'); add('SET', 't', '1'); add('INCR', 't'); add('INCR', 'kk'); add('EXEC');
  add('MULTI'); add('GET'); add('SET', 't', '2'); add('EXEC'); add('GET', 't');
  add('EXEC'); add('DISCARD'); add('WATCH', 't'); add('MULTI'); add('GET', 't'); add('EXEC'); add('UNWATCH');
  // scripting
  add('EVAL', 'return 1', '0'); add('EVAL', 'return redis.call("GET", KEYS[1])', '1', 't'); add('EVAL', 'return {1,2,{3,"x"}}', '0'); add('EVAL', 'return redis.error_reply("MY ERR")', '0');
  add('EVAL', 'this is not lua', '0'); add('EVAL', 'return redis.call("NOSUCH")', '0'); add('EVAL', 'return tostring(3.7)', '0'); add('EVAL', 'return 3.7', '0'); add('EVAL', 'return cjson.encode({a=1})', '0');
  add('SCRIPT', 'LOAD', 'return ARGV[1]'); add('EVALSHA', 'bogussha', '0'); add('EVAL', 'redis.setresp(3); return redis.call("HGETALL", KEYS[1])', '1', 'h');
  add('FUNCTION', 'LOAD', "#!lua name=mylib\nredis.register_function('f1', function(keys, args) return args[1] end)"); add('FCALL', 'f1', '0', 'hello'); add('FCALL', 'nope', '0'); add('FUNCTION', 'LIST');
  // RESP3 switch + repeats of container replies
  add('HELLO', '3'); add('HGETALL', 'h'); add('ZRANGE', 'zu', '0', '-1', 'WITHSCORES'); add('SMEMBERS', 's2'); add('ZSCORE', 'z2', 'x'); add('GET', 'nokey'); add('EXISTS', 'h');
  add('EVAL', 'return {1.5, true, false}', '0'); add('EVAL', 'redis.setresp(3); return redis.call("ZSCORE", KEYS[1], "x")', '1', 'z2'); add('HELLO', '2'); add('HGETALL', 'h');
  // misc
  add('DBSIZE'); add('SELECT', '1'); add('DBSIZE'); add('SET', 'db1', 'x'); add('SWAPDB', '0', '1'); add('DBSIZE'); add('SELECT', '0'); add('DBSIZE'); add('FLUSHDB'); add('DBSIZE'); add('SELECT', '1');
  add('COMMAND', 'COUNT'); add('COMMAND', 'INFO', 'GET'); add('COMMAND', 'DOCS', 'GET'); add('CONFIG', 'GET', 'maxmemory'); add('CONFIG', 'GET', 'io-threads'); add('ACL', 'WHOAMI'); add('ACL', 'LIST');
  add('CLIENT', 'GETNAME'); add('CLIENT', 'SETNAME', 'tryme'); add('CLIENT', 'GETNAME'); add('CLIENT', 'ID'); add('CLIENT', 'INFO');
  add('SHUTDOWN', 'BOGUS'); add('DEBUG', 'BOGUS'); add('OBJECT', 'HELP'); add('MEMORY', 'DOCTOR'); add('LOLWUT', 'VERSION', '5', '3', '3');
  // seeded random ops on a bounded keyspace
  const rnd = mulberry32(0xC0FFEE);
  const pick = (arr) => arr[Math.floor(rnd() * arr.length)];
  const keys = ['rk1', 'rk2', 'rk3', 'rh', 'rl', 'rs', 'rz'];
  for (let i = 0; i < 400; i++) {
    const k = pick(keys); const v = String(Math.floor(rnd() * 1000)); const f = 'f' + Math.floor(rnd() * 8);
    switch (Math.floor(rnd() * 16)) {
      case 0: add('SET', k, v); break;
      case 1: add('GET', k); break;
      case 2: add('INCRBY', k, v); break;
      case 3: add('HSET', k, f, v); break;
      case 4: add('HGETALL', k); break;
      case 5: add('RPUSH', k, v, f); break;
      case 6: add('LRANGE', k, '0', '-1'); break;
      case 7: add('LPOP', k); break;
      case 8: add('SADD', k, f); break;
      case 9: add('SMEMBERS', k); break;
      case 10: add('ZADD', k, v, f); break;
      case 11: add('ZRANGE', k, '0', '-1', 'WITHSCORES'); break;
      case 12: add('DEL', k); break;
      case 13: add('TYPE', k); break;
      case 14: add('OBJECT', 'ENCODING', k); break;
      case 15: add('APPEND', k, f); break;
    }
  }
  return c;
}

// ---------- run ----------
const corpus = buildCorpus();
const w = await startWasm();
const n = await startNative();
let ok = 0, bad = 0;
const show = (b) => JSON.stringify(b.toString('latin1'));
for (const args of corpus) {
  const [a, b] = await Promise.all([w.call(args), n.call(args)]);
  if (Buffer.compare(a, b) === 0) ok++;
  else {
    bad++;
    console.log(`MISMATCH: ${args.map((x) => JSON.stringify(x)).join(' ')}\n  wasm:   ${show(a)}\n  native: ${show(b)}`);
  }
}
n.close();
console.log(`\n${ok} identical, ${bad} mismatched, ${corpus.length} total`);
process.exit(bad ? 1 : 0);
