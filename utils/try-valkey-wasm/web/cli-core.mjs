// cli-core.mjs -- the parts of valkey-cli that live in JS for try-valkey.
//
// Two responsibilities, both ported line-for-line from src/valkey-cli.c so
// the terminal output matches the real tool:
//   * parseRESP(bytes)     -> reply tree (RESP2 + RESP3)
//   * formatReplyTTY(reply) -> text, a port of cliFormatReplyTTY()
// plus the small amount of session state the real CLI tracks to render its
// prompt (SELECTed db, open MULTI, subscribed mode, RESP version).
//
// Command-line splitting is NOT here: it is done by sdssplitargs() inside the
// wasm module (tv_encode_command), so quoting rules are the server's own.

// ---------------------------------------------------------------- parsing --
// Reply node: { type, str?, int?, elements? }
// type is one of: status error integer double string verb nil bool bignum
//                 array map set push attr

export function parseRESP(buf, start = 0) {
  // Returns [node, nextOffset] or null if the buffer holds an incomplete reply.
  if (start >= buf.length) return null;
  const t = String.fromCharCode(buf[start]);
  const eol = indexOfCRLF(buf, start);
  if (eol < 0) return null;
  const line = latin1(buf, start + 1, eol);
  let off = eol + 2;
  const simple = (type, extra) => [{ type, ...extra }, off];
  switch (t) {
    case '+': return simple('status', { str: line });
    case '-': return simple('error', { str: line });
    case ':': return simple('integer', { int: line });
    case '_': return simple('nil', {});
    case ',': return simple('double', { str: line });
    case '#': return simple('bool', { int: line === 't' ? 1 : 0 });
    case '(': return simple('bignum', { str: line });
    case '$': case '!': case '=': {
      const n = parseInt(line, 10);
      if (n < 0) return simple('nil', {});
      if (off + n + 2 > buf.length) return null;
      let str = latin1(buf, off, off + n);
      off += n + 2;
      if (t === '=') return [{ type: 'verb', str: str.slice(4) }, off]; // strip "txt:"
      if (t === '!') return [{ type: 'error', str }, off];
      return [{ type: 'string', str }, off];
    }
    case '*': case '~': case '>': case '%': case '|': {
      const n = parseInt(line, 10);
      if (n < 0) return simple('nil', {});
      const count = (t === '%' || t === '|') ? n * 2 : n;
      const elements = [];
      for (let i = 0; i < count; i++) {
        const r = parseRESP(buf, off);
        if (!r) return null;
        elements.push(r[0]); off = r[1];
      }
      const type = { '*': 'array', '~': 'set', '>': 'push', '%': 'map', '|': 'attr' }[t];
      if (type === 'attr') { // attributes decorate the following value; drop them like libvalkey does
        return parseRESP(buf, off);
      }
      return [{ type, elements }, off];
    }
    default:
      throw new Error(`Protocol error: unexpected type byte ${JSON.stringify(t)}`);
  }
}

function indexOfCRLF(buf, from) {
  for (let i = from; i + 1 < buf.length; i++) if (buf[i] === 13 && buf[i + 1] === 10) return i;
  return -1;
}
function latin1(buf, a, b) {
  let s = '';
  for (let i = a; i < b; i++) s += String.fromCharCode(buf[i]);
  return s;
}

// ------------------------------------------------------------- formatting --

// sdscatrepr(): quote a byte string the way valkey-cli prints bulk strings.
export function repr(str) {
  let out = '"';
  for (let i = 0; i < str.length; i++) {
    const c = str.charCodeAt(i);
    const ch = str[i];
    if (c >= 0x20 && c <= 0x7e && ch !== '\\' && ch !== '"') out += ch;
    else if (ch === '\\' || ch === '"') out += '\\' + ch;
    else if (ch === '\n') out += '\\n';
    else if (ch === '\r') out += '\\r';
    else if (ch === '\t') out += '\\t';
    else if (c === 7) out += '\\a';
    else if (c === 8) out += '\\b';
    else out += '\\x' + c.toString(16).padStart(2, '0');
  }
  return out + '"';
}

function isMultilineValueTTY(r) {
  switch (r.type) {
    case 'array': case 'set': case 'push':
      if (r.elements.length === 0) return false;
      if (r.elements.length > 1) return true;
      return isMultilineValueTTY(r.elements[0]);
    case 'map':
      if (r.elements.length === 0) return false;
      if (r.elements.length > 2) return true;
      return isMultilineValueTTY(r.elements[1]);
    default: return false;
  }
}

// Port of cliFormatReplyTTY(r, prefix). Returns text ending in "\n".
export function formatReplyTTY(r, prefix = '') {
  switch (r.type) {
    case 'error': return `(error) ${r.str}\n`;
    case 'status': return `${r.str}\n`;
    case 'integer': return `(integer) ${r.int}\n`;
    case 'double': return `(double) ${r.str}\n`;
    case 'string': return `${repr(r.str)}\n`;
    case 'verb': return `${r.str}\n`;
    case 'nil': return '(nil)\n';
    case 'bool': return r.int ? '(true)\n' : '(false)\n';
    case 'bignum': return `(big number) ${r.str}\n`; // valkey-cli has no case for this; keep it readable
    case 'array': case 'map': case 'set': case 'push': {
      const n = r.elements.length;
      if (n === 0) {
        return { array: '(empty array)\n', map: '(empty hash)\n', set: '(empty set)\n', push: '(empty push)\n' }[r.type];
      }
      // chars needed to represent the largest index
      let i = r.type === 'map' ? Math.floor(n / 2) : n;
      let idxlen = 0;
      do { idxlen++; i = Math.floor(i / 10); } while (i);
      const childPrefix = prefix + ' '.repeat(idxlen + 2);
      const numsep = r.type === 'set' ? '~' : r.type === 'map' ? '#' : ')';
      let out = '';
      for (let k = 0; k < n; k++) {
        const humanIdx = (r.type === 'map' ? Math.floor(k / 2) : k) + 1;
        out += (k === 0 ? '' : prefix) + String(humanIdx).padStart(idxlen, ' ') + numsep + ' ';
        out += formatReplyTTY(r.elements[k], childPrefix);
        if (r.type === 'map') {
          k++;
          out = out.slice(0, -1) + ' => ';
          if (isMultilineValueTTY(r.elements[k])) out += '\n' + childPrefix;
          out += formatReplyTTY(r.elements[k], childPrefix);
        }
      }
      return out;
    }
    default: throw new Error(`Unknown reply type: ${r.type}`);
  }
}

// ------------------------------------------------------------ CLI session --

export function isPubsubPush(r, resp3) {
  if (!r || r.type !== (resp3 ? 'push' : 'array') || r.elements.length < 3) return false;
  const first = r.elements[0];
  if (first.type !== 'string') return false;
  return first.str.endsWith('message') || first.str.endsWith('subscribe');
}

/* Tracks what valkey-cli tracks to draw its prompt and interpret replies. */
export class CliSession {
  constructor(hostLabel = 'try-valkey') {
    this.hostLabel = hostLabel;
    this.dbnum = 0;
    this.inMulti = false;
    this.pubsubMode = false;
    this.resp3 = false;
  }

  prompt() {
    let p = this.hostLabel;
    if (this.dbnum !== 0) p += `[${this.dbnum}]`;
    if (this.inMulti) p += '(TX)';
    if (this.pubsubMode) p += '(subscribed mode)';
    return p + '> ';
  }

  /* Mirror of the state updates in cliSendCommand()/cliReadReply(). */
  observe(argv, reply) {
    const cmd = (argv[0] || '').toLowerCase();
    const ok = reply.type === 'status';
    if (cmd === 'select' && argv.length === 2 && ok) this.dbnum = parseInt(argv[1], 10) || 0;
    if (cmd === 'multi' && ok) this.inMulti = true;
    if ((cmd === 'exec' || cmd === 'discard') && reply.type !== 'error') this.inMulti = false;
    if (cmd === 'exec' && reply.type === 'error' && /EXECABORT/.test(reply.str)) this.inMulti = false;
    if (cmd === 'reset' && ok) { this.dbnum = 0; this.inMulti = false; this.pubsubMode = false; this.resp3 = false; }
    if (cmd === 'hello' && reply.type === 'map') {
      const i = reply.elements.findIndex((e) => e.type === 'string' && e.str === 'proto');
      if (i >= 0) this.resp3 = reply.elements[i + 1].int === '3';
    }
    if (isPubsubPush(reply, this.resp3) && reply.elements[0].str.endsWith('subscribe')) {
      this.pubsubMode = reply.elements[2].type === 'integer' && parseInt(reply.elements[2].int, 10) > 0;
    }
  }
}
