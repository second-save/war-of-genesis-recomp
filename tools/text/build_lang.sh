#!/bin/sh
# Rebuild the translation overlay: tools/text/build_lang.sh [GAME_DIR] [LANG]
set -e
GAME=${1:-The-War-of-Genesis_DOS_KO}
L=${2:-en}
T=translations/$L
O=lang/$L
PY=${PYTHON:-python3}
$PY tools/text/visual.py build "$GAME" "$T" "$O"
$PY tools/text/prologue.py build "$GAME" "$T" "$O"
$PY tools/text/talk.py build "$GAME" "$T" "$O"
$PY tools/text/strings.py build "$T" "$O"
