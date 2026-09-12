// cli-hints.mjs -- valkey-cli's Tab completion and inline argument hints, in JS.
//
// A port of the linenoise callbacks in src/valkey-cli.c (completionCallback,
// hintsCallback and the matchArgs/addHintForArguments machinery behind them),
// fed by the real server's COMMAND DOCS reply. Kept structurally close to the
// C so behaviour matches: same optional-argument grouping rule, same
// "don't match the last word until a space follows it" rule, same brackets.

const OPTIONAL = 1, MULTIPLE = 2, MULTIPLE_TOKEN = 4;

/* ---- building the help table from COMMAND DOCS ------------------------- */

function mapEntries(node) {
  // COMMAND DOCS is a map in RESP3, a flat array of key/value pairs in RESP2
  const out = [];
  const e = node.elements || [];
  for (let i = 0; i + 1 < e.length; i += 2) out.push([e[i].str, e[i + 1]]);
  return out;
}

function makeArg(argMap) {
  const a = { name: null, display_text: null, token: null, type: 'string', flags: 0, subargs: [] };
  for (const [k, v] of mapEntries(argMap)) {
    if (k === 'name') a.name = v.str;
    else if (k === 'display_text') a.display_text = v.str;
    else if (k === 'token') a.token = v.str;
    else if (k === 'type') a.type = v.str;
    else if (k === 'arguments') a.subargs = (v.elements || []).map(makeArg);
    else if (k === 'flags') for (const f of v.elements || []) {
      if (f.str === 'optional') a.flags |= OPTIONAL;
      else if (f.str === 'multiple') a.flags |= MULTIPLE;
      else if (f.str === 'multiple_token') a.flags |= MULTIPLE_TOKEN;
    }
  }
  return a;
}

/* Returns [{argv: ['SET'], full: 'SET', args: [...], params, summary, group}] for every
 * command and subcommand (subcommand names arrive as "container|sub"). */
export function buildHelpEntries(docsReply) {
  const entries = [];
  const add = (cmdname, subname, specs) => {
    const argv = [cmdname.toUpperCase()];
    if (subname) argv.push(subname.includes('|') ? subname.slice(subname.indexOf('|') + 1).toUpperCase() : subname.toUpperCase());
    const he = { argv, full: argv.join(' '), args: null, params: '', summary: '', group: '' };
    for (const [k, v] of mapEntries(specs)) {
      if (k === 'summary') he.summary = v.str;
      else if (k === 'group') he.group = v.str;
      else if (k === 'arguments') { he.args = (v.elements || []).map(makeArg); he.params = makeHint(null, 0, 0, he); }
      else if (k === 'subcommands') for (const [sn, sv] of mapEntries(v)) add(cmdname, sn, sv);
    }
    entries.push(he);
  };
  for (const [name, specs] of mapEntries(docsReply)) add(name, null, specs);
  // COMMAND DOCS comes back in hash order (different on every server boot); sort so Tab cycling is predictable
  entries.sort((a, b) => (a.full < b.full ? -1 : a.full > b.full ? 1 : 0));
  return entries;
}

/* ---- matching typed words against the argument tree --------------------- */

function clearMatched(args) {
  for (const a of args) { a.matched = 0; a.matched_token = 0; a.matched_name = 0; a.matched_all = 0; if (a.subargs.length) clearMatched(a.subargs); }
}

function matchNoTokenArg(words, arg) {
  switch (arg.type) {
    case 'block': {
      arg.matched += matchArgs(words, arg.subargs);
      arg.matched_all = arg.subargs.every((s) => s.matched_all) ? 1 : 0;
      break;
    }
    case 'oneof': {
      for (const s of arg.subargs) {
        if (matchArg(words, s)) { arg.matched += s.matched; arg.matched_all = s.matched_all; break; }
      }
      break;
    }
    case 'integer': case 'unix-time':
      if (/^\s*[-+]?\d+/.test(words[0])) { arg.matched += 1; arg.matched_name = 1; arg.matched_all = 1; }
      else { arg.matched = 0; arg.matched_name = 0; }
      break;
    case 'double':
      if (/^\s*[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?|^\s*[-+]?(inf|nan)/i.test(words[0])) { arg.matched += 1; arg.matched_name = 1; arg.matched_all = 1; }
      else { arg.matched = 0; arg.matched_name = 0; }
      break;
    default:
      arg.matched += 1; arg.matched_name = 1; arg.matched_all = 1;
  }
  return arg.matched;
}

function matchArgOnce(words, arg) {
  if (arg.token !== null) {
    if (arg.token.toLowerCase() !== words[0].toLowerCase()) return 0;
    arg.matched_token = 1; arg.matched = 1;
    if (arg.type === 'pure-token') { arg.matched_all = 1; return 1; }
    if (words.length === 1) return 1;
    words = words.slice(1);
  }
  if (!matchNoTokenArg(words, arg)) return 0;
  return arg.matched;
}

function matchArg(words, arg) {
  let matchedOnce = matchArgOnce(words, arg);
  if (!(arg.flags & MULTIPLE)) return matchedOnce;
  let matchedWords = matchedOnce;
  while (arg.matched_all && matchedWords < words.length) {
    clearMatched([arg]);
    if (arg.token !== null && !(arg.flags & MULTIPLE_TOKEN)) {
      matchedOnce = matchNoTokenArg(words.slice(matchedWords), arg);
      if (arg.matched) arg.matched_token = 1;
    } else {
      matchedOnce = matchArgOnce(words.slice(matchedWords), arg);
    }
    matchedWords += matchedOnce;
  }
  arg.matched_all = 0; // more repetitions are still possible
  return matchedWords;
}

function matchOneOptionalArg(words, args) {
  for (let nextarg = 0; words.length && nextarg < args.length; nextarg++) {
    if (args[nextarg].matched) continue;
    const n = matchArg(words, args[nextarg]);
    if (n) return [n, nextarg];
  }
  return [0, -1];
}

function matchOptionalArgs(words, args) {
  let nextword = 0, last = -1;
  while (nextword < words.length) {
    const [n, idx] = matchOneOptionalArg(words.slice(nextword), args);
    if (!n) break;
    if (last !== -1) args[last].matched_all = 1;
    last = idx;
    nextword += n;
  }
  return nextword;
}

function matchArgs(words, args) {
  let nextword = 0;
  for (let nextarg = 0; nextword < words.length && nextarg < args.length; nextarg++) {
    let n;
    if (args[nextarg].flags & OPTIONAL) {
      let last = nextarg;
      while (last < args.length && (args[last].flags & OPTIONAL)) last++;
      n = matchOptionalArgs(words.slice(nextword), args.slice(nextarg, last));
      nextarg = last - 1;
    } else {
      n = matchArg(words.slice(nextword), args[nextarg]);
      if (!n) return 0; // a required word could not be matched
    }
    nextword += n;
  }
  return nextword;
}

/* ---- rendering the remaining syntax ------------------------------------- */

const orEmpty = (s) => (s === '' ? '""' : s);

function addHintForArguments(hint, args, sep) {
  let len = hint.length;
  const addSep = (h, isLast) => { if (h.length > len && !isLast) { h += sep; len = h.length; } return h; };
  for (let i = 0; i < args.length; i++) {
    if (!(args[i].flags & OPTIONAL)) { hint = addSep(addHintForArgument(hint, args[i]), i === args.length - 1); continue; }
    // successive optional args may appear in any order; show a partially typed one first
    let j = i, incomplete = -1;
    for (; j < args.length; j++) {
      if (!(args[j].flags & OPTIONAL)) break;
      if (args[j].matched !== 0 && args[j].matched_all === 0) { hint = addSep(addHintForArgument(hint, args[j]), i === args.length - 1); incomplete = j; }
    }
    if (j === args.length || args[j].matched === 0) {
      for (; i < j; i++) if (incomplete !== i) hint = addSep(addHintForArgument(hint, args[i]), i === args.length - 1);
    }
    i = j - 1;
  }
  return hint;
}

function addHintForRepeatedArgument(hint, arg) {
  if (!(arg.flags & MULTIPLE)) return hint;
  clearMatched([arg]);
  if (hint !== '') hint += ' ';
  hint += '[';
  if (arg.flags & MULTIPLE_TOKEN) { hint += orEmpty(arg.token); if (arg.type !== 'pure-token') hint += ' '; }
  switch (arg.type) {
    case 'oneof': hint = addHintForArguments(hint, arg.subargs, '|'); break;
    case 'block': hint = addHintForArguments(hint, arg.subargs, ' '); break;
    case 'pure-token': break;
    default: hint += orEmpty(arg.display_text ?? arg.name);
  }
  return hint + ' ...]';
}

function addHintForArgument(hint, arg) {
  if (arg.matched_all) return hint;
  if ((arg.flags & OPTIONAL) && !arg.matched) hint += '[';
  if (arg.token !== null && !arg.matched_token) { hint += orEmpty(arg.token); if (arg.type !== 'pure-token') hint += ' '; }
  switch (arg.type) {
    case 'oneof':
      if (arg.matched === 0) hint = addHintForArguments(hint, arg.subargs, '|');
      else for (const s of arg.subargs) if (s.matched !== 0) hint = addHintForArgument(hint, s);
      break;
    case 'block': hint = addHintForArguments(hint, arg.subargs, ' '); break;
    case 'pure-token': break;
    default: if (!arg.matched_name) hint += orEmpty(arg.display_text ?? arg.name);
  }
  hint = addHintForRepeatedArgument(hint, arg);
  if ((arg.flags & OPTIONAL) && !arg.matched) hint += ']';
  return hint;
}

/* makeHint(inputargv, inputargc, cmdlen, entry): the syntax still to be typed. */
export function makeHint(inputargv, inputargc, cmdlen, entry) {
  if (entry.args) {
    clearMatched(entry.args);
    let matchedWords = 0;
    if (inputargv && inputargc) matchedWords = matchArgs(inputargv.slice(cmdlen, inputargc), entry.args);
    return matchedWords === inputargc - cmdlen ? addHintForArguments('', entry.args, ' ') : '';
  }
  return inputargc <= cmdlen ? entry.params : '';
}

/* Longest command/subcommand prefix matching the typed words. */
export function findHelpEntry(entries, argv) {
  let best = null, bestLen = 0;
  for (const he of entries) {
    if (he.argv.length <= argv.length && he.argv.length > bestLen &&
        he.argv.every((w, i) => w.toLowerCase() === argv[i].toLowerCase())) { best = he; bestLen = he.argv.length; }
  }
  return best;
}

/* ---- the two callbacks ----------------------------------------------------- */

/* hintFor(line, splitArgs): grey text to show after the cursor, or ''. */
export function hintFor(entries, line, splitArgs) {
  if (!line) return '';
  const argv = splitArgs(line);
  if (!argv || !argv.length) return '';
  const endspace = /\s$/.test(line);
  const matchargc = endspace ? argv.length : argv.length - 1; // don't match the last word until a space follows it
  const entry = findHelpEntry(entries, argv.slice(0, matchargc));
  if (!entry) return '';
  const hint = makeHint(argv, matchargc, entry.argv.length, entry);
  if (!hint) return '';
  return (endspace ? '' : ' ') + hint;
}

/* completionsFor(line): full lines that complete the typed prefix, in table order. */
export function completionsFor(entries, line) {
  let startpos = 0;
  if (/^help\s/i.test(line)) { startpos = 5; while (/\s/.test(line[startpos] || '')) startpos++; }
  const prefix = line.slice(startpos).toLowerCase();
  const out = [];
  for (const he of entries) if (he.full.toLowerCase().startsWith(prefix)) out.push(line.slice(0, startpos) + he.full);
  return out;
}
