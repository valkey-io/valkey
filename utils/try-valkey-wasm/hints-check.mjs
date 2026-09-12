// hints-check.mjs -- exercise cli-hints against the real COMMAND DOCS of the wasm server.
import createValkey from '../../src/valkey-server.mjs';
import { startTryValkey } from './web/tryvalkey.mjs';
import { buildHelpEntries, hintFor, completionsFor } from './web/cli-hints.mjs';

const tv = await startTryValkey(createValkey);
const docs = await tv.execRaw('command docs');
const entries = buildHelpEntries(docs);
console.log(`entries: ${entries.length} (commands + subcommands)`);

const split = (l) => tv.splitArgs(l);
const cases = [
  'SET', 'SET ', 'SET k', 'SET k ', 'SET k v ', 'SET k v NX ', 'SET k v EX ', 'SET k v EX 10 ', 'set k v ex 10 nx ',
  'GET ', 'HSET h ', 'HSET h f v ', 'ZADD z ', 'ZADD z 1 a ', 'ZRANGE z 0 -1 ', 'LPUSH l a b ',
  'CLIENT ', 'CLIENT LIST ', 'CLIENT KILL ', 'CONFIG SET ', 'COMMAND DOCS ', 'EXPIRE k ', 'EXPIRE k 10 ',
  'BLPOP ', 'XADD s ', 'SCAN 0 ', 'EVAL ', 'HELLO ', 'PING ', 'INFO ', 'OBJECT ENCODING ',
];
for (const c of cases) console.log(JSON.stringify(c).padEnd(22), '->', JSON.stringify(hintFor(entries, c, split)));

console.log('\ncompletions:');
for (const c of ['SE', 'hget', 'client k', 'ZR', 'help s']) console.log(JSON.stringify(c).padEnd(12), '->', completionsFor(entries, c).slice(0, 8).join(' | '), completionsFor(entries, c).length > 8 ? `... (${completionsFor(entries, c).length})` : '');
