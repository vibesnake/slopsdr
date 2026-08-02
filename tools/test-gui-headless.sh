#!/bin/sh

# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 vibesnake

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

if [ "$#" -ge 1 ]; then
    case "$1" in
        /*) BUILD_DIR=$1 ;;
        *)  BUILD_DIR="$REPO_ROOT/$1" ;;
    esac
else
    BUILD_DIR="$REPO_ROOT/build/desktop-tests"
fi

if [ "$#" -ge 1 ]; then
    shift
fi

if [ ! -d "$BUILD_DIR" ]; then
    printf 'Build directory not found: %s\n' "$BUILD_DIR" >&2
    exit 1
fi

exec dbus-run-session -- \
    xvfb-run -a \
    -s "-screen 0 1920x1080x24" \
    env \
        QT_QPA_PLATFORM=xcb \
        QT_X11_NO_MITSHM=1 \
        ctest \
            --test-dir "$BUILD_DIR" \
            --output-on-failure \
            "$@"
