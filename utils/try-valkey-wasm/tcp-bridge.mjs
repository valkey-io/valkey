// tcp-bridge.mjs -- expose the in-process wasm server on a TCP port.
//
// Each TCP connection becomes one mem connection (tv_connect); bytes are
// shuttled both ways and the event loop is ticked after every write and on a
// short timer. This exists so the stock Tcl test-suite can run against the
// wasm build in external mode:
//
//   node tcp-bridge.mjs --port 7379 [extra valkey-server args...] &
//   ./runtest --host 127.0.0.1 --port 7379 --singledb --tags "-needs:repl ..."
import net from 'node:net';
import createValkey from '../../src/valkey-server.mjs';

const argv = process.argv.slice(2);
let port = 7379;
const serverArgs = [];
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === '--port') { port = Number(argv[++i]); continue; }
  serverArgs.push(argv[i]);
}

const Module = await createValkey({
  print: (s) => process.stdout.write(s + '\n'),
  printErr: (s) => process.stderr.write(s + '\n'),
  noInitialRun: true,
});
try {
  Module.callMain(['--port', '0', '--save', '', '--appendonly', 'no', '--maxclients', '800',
    '--loglevel', 'notice', ...serverArgs]);
} catch (e) { if (e !== 'unwind' && e?.name !== 'ExitStatus') throw e; }

const CAP = 1 << 20;
const buf = Module._malloc(CAP);
const bufP = BigInt(buf);
const conns = new Map(); // id -> socket

function tick() {
  Module._tv_tick();
  for (const [id, sock] of conns) {
    const pending = Module._tv_pending(id);
    if (pending < 0) { conns.delete(id); sock.destroy(); continue; } // server closed it
    while (Module._tv_pending(id) > 0) {
      const n = Module._tv_read(id, bufP, CAP);
      if (n <= 0) break;
      sock.write(Buffer.from(Module.HEAPU8.buffer, Number(buf), n)); // copies
    }
  }
}

const server = net.createServer((sock) => {
  sock.setNoDelay(true);
  const id = Module._tv_connect();
  if (id < 0) { sock.destroy(); return; }
  conns.set(id, sock);
  sock.on('data', (d) => {
    if (!conns.has(id)) return;
    for (let off = 0; off < d.length; off += CAP) {
      const chunk = d.subarray(off, Math.min(off + CAP, d.length));
      Module.HEAPU8.set(chunk, Number(buf));
      Module._tv_write(id, bufP, chunk.length);
    }
    tick();
  });
  const bye = () => { if (conns.delete(id)) { Module._tv_close(id); tick(); } };
  sock.on('close', bye);
  sock.on('error', bye);
});
server.listen(port, '127.0.0.1', () => {
  process.stdout.write(`tcp-bridge: wasm valkey-server on 127.0.0.1:${port}\n`);
});
setInterval(tick, 10);
