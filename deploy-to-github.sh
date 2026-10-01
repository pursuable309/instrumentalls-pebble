#!/usr/bin/env bash
#
# Deploy the pebble/ subtree to a standalone GitHub repo whose ROOT is the
# Pebble project, so CloudPebble's "Import from GitHub" can build it.
#
# Why: CloudPebble needs package.json / src/ / wscript at the repo ROOT, but in
# this project they live under pebble/. Rather than paste main.c into the mobile
# editor (Android's clipboard truncates at ~500 lines, which silently drops the
# tail of the file and breaks the build), we publish the committed pebble/ tree
# as a one-commit mirror. The main instrumentalls repo stays the single source
# of truth; the mirror is always overwritten from HEAD, so never edit it directly.
#
# Uses only core git plumbing (commit-tree) — no `git subtree`, no second
# checkout, no rsync.
#
# ── One-time setup ───────────────────────────────────────────────────────────
# Public repo is fine: this app has NO secrets (AUTH_TOKEN is empty, BACKEND_URL
# is localhost). Then, from the project root:
#   1. Create an EMPTY GitHub repo, e.g. instrumentalls-pebble.
#   2. git remote add pebble-deploy https://github.com/<you>/instrumentalls-pebble.git
#
# ── Each update ──────────────────────────────────────────────────────────────
#   git add pebble/ && git commit -m "..."     # pebble/ must be committed first
#   bash pebble/deploy-to-github.sh
#   -> The linked CloudPebble project auto-builds on push; open the project
#      (cloudpebble.repebble.com/ide/project/30205) and download the .pbw.
#      First time only: CloudPebble Import from GitHub with the mirror URL.
#
# Overrides: PEBBLE_DEPLOY_REMOTE (default "pebble-deploy"),
#            PEBBLE_DEPLOY_BRANCH (default "main").
#
set -euo pipefail

REMOTE="${PEBBLE_DEPLOY_REMOTE:-pebble-deploy}"
BRANCH="${PEBBLE_DEPLOY_BRANCH:-main}"
PREFIX="pebble"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"

if ! git remote get-url "$REMOTE" >/dev/null 2>&1; then
  echo "error: no '$REMOTE' remote configured. Add it once:" >&2
  echo "  git remote add $REMOTE https://github.com/<you>/instrumentalls-pebble.git" >&2
  exit 1
fi

# The deploy publishes HEAD's pebble/ tree, so pebble/ must be fully committed
# (this also enforces the main repo as the single source of truth).
if [ -n "$(git status --porcelain -- "$PREFIX")" ]; then
  echo "error: uncommitted changes under $PREFIX/ — commit them first:" >&2
  git status --short -- "$PREFIX" >&2
  exit 1
fi

# Publish HEAD's pebble/ tree verbatim as the mirror root. The JS source is a
# normal src/pkjs/index.js, so the tree is used as-is (no rename step).
tree="$(git rev-parse "HEAD:$PREFIX")"
commit="$(git commit-tree "$tree" \
  -m "deploy pebble app from ${PWD##*/}@$(git rev-parse --short HEAD)")"

echo "Publishing $PREFIX/ (tree $tree) -> $REMOTE/$BRANCH ..."
git push --force "$REMOTE" "$commit:refs/heads/$BRANCH"
echo "Done. In CloudPebble: Import from GitHub (first time), else Run build."
