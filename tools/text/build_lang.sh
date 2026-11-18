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
$PY tools/text/battle.py build "$GAME" "$T" "$O"
$PY tools/text/items.py build "$GAME" "$T" "$O"
cat "$O/TEXT_TAK.TXT" "$O/TEXT_SCE.TXT" > "$O/TEXT.TXT"
$PY tools/text/strings.py build "$T" "$O"
# Images with text, rendered by tools/gfx (committed, only rendered text)
mkdir -p "$O/CHAPDATA" && cp "$T"/CHAPDATA/*.VSP "$O/CHAPDATA/"
$PY tools/gfx/ttl_pages.py build "$GAME" "$T" "$O"
$PY tools/gfx/ttl_staff.py build "$GAME" "$T" "$O"
$PY tools/gfx/story_scroll.py build "$GAME" "$T" "$O"
$PY tools/gfx/special_icons.py build "$GAME" "$T" "$O"
