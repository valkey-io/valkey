// Boot probe: start the wasm valkey-server under Node and see how far it gets.
import createValkey from '../../src/valkey-server.mjs';

const args = process.argv.slice(2);
const Module = await createValkey({
  print: (s) => console.log('[out]', s),
  printErr: (s) => console.log('[err]', s),
  noInitialRun: true,
});
console.log('sizeof(void*) probe: memory64 lowered build loaded OK');
try {
  const rc = Module.callMain(args.length ? args : ['--port', '0']);
  console.log('main returned', rc);
} catch (e) {
  console.log('main threw', e && e.name, e && e.message);
}
