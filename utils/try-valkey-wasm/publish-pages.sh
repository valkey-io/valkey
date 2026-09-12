#!/usr/bin/env bash
# Publish the try-valkey web bundle to the fork's gh-pages branch (GitHub Pages).
#
#   utils/try-valkey-wasm/publish-pages.sh            # from a tree where build.sh has produced src/valkey-server.{mjs,wasm}
#
# The site is https://valkey-rainfall.github.io/valkey/  (tryme.html, index.html).
# The gh-pages branch is an orphan holding only the deployable files, so build
# artifacts never enter the source branches' history.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
REMOTE="${REMOTE:-origin}"
SRC_SHA="$(git -C "$ROOT" rev-parse --short HEAD)"

for f in "$ROOT/src/valkey-server.mjs" "$ROOT/src/valkey-server.wasm"; do
  [[ -f "$f" ]] || { echo "missing $f -- run build.sh first" >&2; exit 1; }
done

WT="$(mktemp -d "${TMPDIR:-/tmp}/try-valkey-pages.XXXXXX")"
trap 'git -C "$ROOT" worktree remove --force "$WT" >/dev/null 2>&1 || rm -rf "$WT"' EXIT

git -C "$ROOT" fetch -q "$REMOTE" gh-pages 2>/dev/null || true
if git -C "$ROOT" rev-parse -q --verify "$REMOTE/gh-pages" >/dev/null; then
  git -C "$ROOT" worktree add -q --detach "$WT" "$REMOTE/gh-pages"
  git -C "$WT" checkout -q -B gh-pages
  git -C "$WT" rm -rq --ignore-unmatch . >/dev/null
else
  git -C "$ROOT" worktree add -q --detach "$WT" HEAD
  git -C "$WT" checkout -q --orphan gh-pages
  git -C "$WT" rm -rfq .
fi

cp "$HERE"/web/*.html "$HERE"/web/*.mjs "$ROOT/src/valkey-server.mjs" "$ROOT/src/valkey-server.wasm" "$WT/"
touch "$WT/.nojekyll"
cat > "$WT/README.md" <<EOF
# try-valkey (GitHub Pages branch)

Built artifacts for the try-valkey page: a real \`valkey-server\` compiled to WebAssembly, running in the browser.
Source and build scripts: branch \`exp/try-valkey-wasm\`, \`utils/try-valkey-wasm/\`. This branch holds only the
deployable bundle; \`valkey-server.wasm\` / \`.mjs\` are build outputs of that branch's \`build.sh\`.

- \`tryme.html\` -- the sign-on animation, handing off to the terminal
- \`index.html\` -- the plain terminal

Published from $SRC_SHA.
EOF

git -C "$WT" add -A
if git -C "$WT" diff --cached --quiet; then echo "gh-pages: nothing changed"; exit 0; fi
git -C "$WT" -c user.name="Rain Valentine" -c user.email="rsg000@gmail.com" commit -q -s -m "try-valkey: publish bundle from $SRC_SHA"
git -C "$WT" push -q "$REMOTE" gh-pages
echo "published $(git -C "$WT" rev-parse --short HEAD) -> https://valkey-rainfall.github.io/valkey/tryme.html"
