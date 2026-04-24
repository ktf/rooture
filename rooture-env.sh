#!/bin/bash
# Wrapper that loads the rooture direnv environment and runs rooture.
# Used e.g. as the MCP server command so Claude Code doesn't need direnv active.

ENVRC="$HOME/src/sw/BUILD/rooture-latest/rooture/.envrc"

if [ ! -f "$ENVRC" ]; then
  echo "rooture-env.sh: cannot find $ENVRC" >&2
  exit 1
fi

# source_up is a direnv built-in; stub it out when sourcing manually.
source_up() { :; }

WORK_DIR="${WORK_DIR:-$HOME/src/sw}"
# .envrc uses relative paths anchored to its own directory, so cd there first.
cd "$(dirname "$ENVRC")" || exit 1
# shellcheck disable=SC1090
source "$(basename "$ENVRC")"

# Change to the source directory so that (load examples/foo.rut) resolves
# against the *source* tree, not the stale installed copy.
SOURCE_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SOURCE_DIR" || exit 1

exec rooture "$@"
