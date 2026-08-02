#!/bin/sh

# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 vibesnake

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

failed=0
matches_file=$(mktemp)
trap 'rm -f "$matches_file"' EXIT

rg -n -o '\]\([^)]*\)' README.md docs --glob '*.md' > "$matches_file" || true
while IFS= read -r match; do
    source_file=${match%%:*}
    remainder=${match#*:}
    remainder=${remainder#*:](}
    target=${remainder%)}
    case "$target" in
        ''|'#'*|http://*|https://*|mailto:*|tel:*) continue ;;
    esac
    target=${target%%#*}
    target=${target%%\?*}
    case "$target" in
        /*) candidate=$REPO_ROOT$target ;;
        *) candidate=$(dirname -- "$source_file")/$target ;;
    esac
    if [ ! -e "$candidate" ]; then
        printf '%s: missing local documentation target %s\n' \
            "$source_file" "$target" >&2
        failed=1
    fi
done < "$matches_file"

exit "$failed"
