// cli-diff.mjs -- feed the same command LINES to
//   (a) real valkey-cli --no-raw reading stdin, talking to a native server, and
//   (b) the JS CLI layer (sdssplitargs-in-wasm + cli-core formatter) on the wasm server,
// then compare the text output line by line.
//
// Usage: node cli-diff.mjs <native-src-dir>   (expects valkey-server + valkey-cli there)
import { spawn } from 'node:child_process';
import createValkey from '../../src/valkey-server.mjs';
import { startTryValkey } from './web/tryvalkey.mjs';

const NATIVE = process.argv[2];
if (!NATIVE) { console.error('usage: cli-diff.mjs <native src dir>'); process.exit(2); }

const LINES = [
  'ping', 'PING "hello world"', 'echo', 'set k v', 'get k', 'get', 'GET k extra',
  'set a "x y\\tz"', 'get a', 'set b "unbalanced', "set c 'single \"quoted\"'", 'get c',
  'set bin "\\x01\\xff\\xc3\\xa9\\n\\r\\\\"', 'get bin', 'append bin "\\x00"', 'strlen bin',
  'incr n', 'incrbyfloat n 2.5', 'incr k',
  'mset m1 a m2 b', 'mget m1 m2 nope', 'del m1 m2', 'exists m1',
  'hset h f1 v1 f2 "with space" f3 ""', 'hgetall h', 'hget h nope', 'hkeys h', 'hmget h f1 zz',
  'rpush l 1 2 3', 'lrange l 0 -1', 'lpop l', 'lpop l 0', 'lrange l 5 10', 'lpop nokey',
  'sadd s a b c', 'smembers s', 'sismember s a', 'smismember s a z',
  'zadd z 1 a 2 b 3 c', 'zrange z 0 -1 withscores', 'zscore z a', 'zscore z nope', 'zrank z b withscore', 'zpopmin z 5', 'zrange z 0 -1',
  'eval "return {1,{2,{3}}}" 0', 'eval "return {1,2,{3,{}}}" 0', 'eval "return {}" 0', 'eval "return redis.error_reply(\'BOOM here\')" 0',
  'eval "return {1.5, true, false}" 0', 'eval "return cjson.encode({a={1,2}})" 0', 'eval "return {{},{{}}}" 0',
  'eval "return redis.call(\'hgetall\',KEYS[1])" 1 h',
  'multi', 'set t 1', 'incr t', 'get', 'exec', 'multi', 'incr t', 'exec', 'exec', 'discard',
  'select 2', 'set indb2 x', 'dbsize', 'select 0', 'dbsize',
  'hello 3', 'hgetall h', 'hget h nope', 'zadd z 1 a 2 b', 'zrange z 0 -1 withscores', 'zscore z a', 'smembers s', 'exists h',
  'eval "return {1.5, true, false}" 0', 'eval "redis.setresp(3); return redis.call(\'hgetall\',KEYS[1])" 1 h',
  'eval "redis.setresp(3); return redis.call(\'zscore\',KEYS[1],\'a\')" 1 z',
  'hset big a 1 b 2 c 3 d 4 e 5 f 6 g 7 h 8 i 9 j 10 k 11', 'hgetall big', 'hello 2', 'hgetall big',
  'config get maxmemory-policy', 'command info get', 'client setname tryme', 'client getname', 'acl whoami',
  'object encoding h', 'type h', 'type nokey', 'randomkey_not_a_command', 'lolwut version 5 3 3', 'info keyspace',
  'client list', // fields with addr differ; still useful to compare shape -- filtered below
  'function load "#!lua name=lib\\nredis.register_function(\'f\', function(k,a) return a end)"', 'fcall f 0 hi', 'function list', 'function flush',
  'quit',
];

// lines whose output legitimately differs between native and wasm (address, randomness, the try page's own config)
const SKIP_LINES = new Set(['client list', 'lolwut version 5 3 3', 'info keyspace', 'config get maxmemory-policy']);

async function runNative() {
  const port = 20000 + Math.floor(Math.random() * 20000);
  const srv = spawn(`${NATIVE}/valkey-server`, ['--port', String(port), '--bind', '127.0.0.1', '--save', '', '--appendonly', 'no', '--loglevel', 'warning'], { stdio: 'ignore' });
  await new Promise((r) => setTimeout(r, 500));
  const cli = spawn(`${NATIVE}/valkey-cli`, ['-p', String(port), '--no-raw'], { stdio: ['pipe', 'pipe', 'pipe'] });
  let out = '';
  cli.stdout.on('data', (d) => { out += d.toString('latin1'); });
  cli.stderr.on('data', (d) => { out += d.toString('latin1'); });
  // Feed one line at a time and attribute whatever arrives before the next
  // line to it (valkey-cli in stdin mode prints synchronously per line).
  cli.stdin.on('error', () => {});
  const outs = [];
  for (const l of LINES) {
    out = '';
    if (l === 'quit') { outs.push(''); break; }
    cli.stdin.write(l + '\n');
    await new Promise((r) => setTimeout(r, 40));
    outs.push(out);
  }
  cli.stdin.end();
  await Promise.race([new Promise((r) => cli.on('close', r)), new Promise((r) => setTimeout(r, 2000))]);
  cli.kill('SIGKILL');
  srv.kill('SIGKILL');
  return outs;
}

async function runWasm() {
  const tv = await startTryValkey(createValkey);
  const outs = [];
  for (const l of LINES) {
    if (l === 'quit') { outs.push(''); break; } // valkey-cli exits on quit; nothing printed
    outs.push(await tv.exec(l));
  }
  return outs;
}

const [nat, was] = await Promise.all([runNative(), runWasm()]);
let ok = 0, bad = 0, skipped = 0;
for (let i = 0; i < LINES.length; i++) {
  if (SKIP_LINES.has(LINES[i])) { skipped++; continue; }
  const a = nat[i] ?? '<missing>', b = was[i] ?? '<missing>';
  if (a === b) ok++;
  else { bad++; console.log(`MISMATCH for: ${LINES[i]}\n  native: ${JSON.stringify(a)}\n  wasm:   ${JSON.stringify(b)}`); }
}
console.log(`\n${ok} identical, ${bad} mismatched, ${skipped} skipped, ${LINES.length} total`);
process.exit(bad ? 1 : 0);
