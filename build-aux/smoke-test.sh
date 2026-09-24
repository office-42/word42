#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The smoke test the three CI jobs run, and one a person can run by hand
# after a build:
#
#     sh build-aux/smoke-test.sh builddir/src/word42-convert
#
# word42-convert is the engine with no window -- the readers, the
# writers and the layout, without GTK -- so this runs on a runner with no
# display at all.  Every sample is written in every format Word42 writes
# and read back, and its text must come through; every XML part of the
# .docx, .odt and .pptx files written must parse; and the Bible sample
# must lay out to the pages it should.  It is a smoke test, not a test
# suite: it asks only that nothing has come apart, and CLAUDE.md is what
# keeps a unit test from growing here.
#
# The Bible's page count depends on its fonts, Carlito and Caladea (the
# metric twins of Calibri and Cambria).  It is checked against
# W42_BIBLE_PAGES when that is set, against 1708 when fc-list finds both
# fonts, and otherwise only printed.  Whatever the fonts, the Bible must
# come back from .docx, .odt, .rtf, .abw and .html to as many pages as
# it went in.

set -eu

conv=${1:-builddir/src/word42-convert}
[ -x "$conv" ] || conv="$conv.exe"
if [ ! -x "$conv" ]; then
  echo "smoke-test: no such program: ${1:-builddir/src/word42-convert}" >&2
  exit 2
fi
# Relative paths stop meaning anything once we move to the scratch
# directory below.
case "$conv" in
  /* | ?:[/\\]*) ;;
  *) conv="$PWD/$conv" ;;
esac
samples=$(cd "$(dirname "$0")/../samples" && pwd)

# BSD mktemp wants the template spelled out, so both sides get one.
work=$(mktemp -d "${TMPDIR:-/tmp}/word42-smoke.XXXXXX")
trap 'rm -rf "$work"' EXIT
cd "$work"

failed=0
fail () {
  echo "   FAIL: $*" >&2
  failed=1
}

python=
for p in python3 python; do
  if command -v "$p" >/dev/null 2>&1; then
    python=$p
    break
  fi
done

# The text of a document, one paragraph a line, with its white space
# made comparable: the formats differ in whether they keep a tab, a run
# of spaces, a no-break space or a line break inside a paragraph, and
# none of that is what is checked.
nbsp=$(printf '\302\240')
lsep=$(printf '\342\200\250')
text () {
  "$conv" --text "$1" > raw.txt || return 1
  tr -d '\r' < raw.txt | tr '\t' ' ' |
    LC_ALL=C sed "s/$nbsp/ /g; s/$lsep/ /g; s/  */ /g; s/^ //; s/ \$//"
}

# probes FILE: up to a dozen of its lines, spread through it, long enough
# to mean something and holding no picture or note mark (U+FFFC).
probes () {
  LC_ALL=C grep -v "$(printf '\357\277\274')" "$1" |
    awk 'length($0) >= 16 { line[n++] = $0 }
         END { step = n > 12 ? n / 12 : 1
               for (i = 0; i < n; i += step) print line[int(i)] }'
}

# xml_ok FILE: every XML part in the zip parses.
xml_ok () {
  [ -n "$python" ] || return 0
  "$python" - "$1" <<'EOF'
import sys, zipfile, xml.dom.minidom
bad = 0
with zipfile.ZipFile(sys.argv[1]) as z:
    for name in z.namelist():
        if name.endswith(".xml") or name.endswith(".rels"):
            try:
                xml.dom.minidom.parseString(z.read(name))
            except Exception as e:
                print("   %s: %s" % (name, e), file=sys.stderr)
                bad += 1
sys.exit(1 if bad else 0)
EOF
}

pages () {
  "$conv" --pages "$1" | tr -d '\r'
}

[ -n "$python" ] || echo "(no python3: the XML parts go unchecked)"

for src in "$samples"/*.docx "$samples"/*/*.docx "$samples"/*/*.doc \
           "$samples"/*.odt "$samples"/*/*.odt "$samples"/*.rtf \
           "$samples"/*/*.rtf "$samples"/*/*.html "$samples"/*/*.abw; do
  [ -f "$src" ] || continue
  name=${src#"$samples"/}
  echo "== $name"
  if ! text "$src" > source.txt; then
    fail "$name does not read"
    continue
  fi
  probes source.txt > probes.txt
  if [ ! -s probes.txt ]; then
    fail "$name reads to no text"
    continue
  fi
  for fmt in docx odt rtf html abw txt pptx pdf; do
    out=out.$fmt
    rm -f "$out"
    if ! "$conv" "$src" "$out" 2> err.txt; then
      fail "$name to .$fmt: $(cat err.txt)"
      continue
    fi
    case $fmt in
      docx | odt | pptx)
        xml_ok "$out" || fail "$name to .$fmt: an XML part does not parse" ;;
    esac
    case $fmt in
      pdf)
        # Read back only with poppler, and lossy by nature: the file
        # being a PDF is what is asked.
        head -c 5 "$out" | grep -q '^%PDF-' || fail "$name to .pdf is not a PDF"
        continue ;;
      pptx)
        # The slides are the outline, not the document; its first
        # line, the title, is what they must keep.
        head -n 1 probes.txt > want.txt ;;
      *)
        cp probes.txt want.txt ;;
    esac
    if ! text "$out" > back.txt; then
      fail "$name as .$fmt does not read back"
      continue
    fi
    missing=0
    while IFS= read -r line; do
      if ! grep -qF -- "$line" back.txt; then
        [ "$missing" -gt 0 ] || fail "$name through .$fmt lost \"$line\""
        missing=$((missing + 1))
      fi
    done < want.txt
    printf '   %-5s %s\n' "$fmt" "$([ "$missing" -eq 0 ] && echo ok || echo "$missing lines lost")"
  done
done

echo "== the Bible's pages"
bible="$samples/bible-kjv.docx"
n=$(pages "$bible")
want=${W42_BIBLE_PAGES:-}
if [ -z "$want" ] && command -v fc-list >/dev/null 2>&1 &&
   fc-list | grep -q Carlito && fc-list | grep -q Caladea; then
  want=1708
fi
if [ -n "$want" ]; then
  echo "   $n pages, $want wanted"
  [ "$n" = "$want" ] || fail "the Bible lays out to $n pages, not $want"
else
  echo "   $n pages (without Carlito and Caladea, not checked)"
fi
for fmt in docx odt rtf abw html; do
  "$conv" "$bible" "bible.$fmt" || { fail "the Bible to .$fmt"; continue; }
  m=$(pages "bible.$fmt")
  echo "   through .$fmt: $m"
  [ "$m" = "$n" ] || fail "the Bible through .$fmt lays out to $m pages, not $n"
done

# The options, and one that is not.
echo "== the command line"
"$conv" --version | grep -q '^word42-convert ' || fail "--version"
"$conv" --help | grep -q '^usage: word42-convert ' || fail "--help"
if "$conv" --nonsense 2>/dev/null; then
  fail "--nonsense was accepted"
fi
if "$conv" no-such-file.docx out.rtf 2>/dev/null; then
  fail "a file that is not there was read"
fi

if [ "$failed" -eq 0 ]; then
  echo "smoke-test: all good"
else
  echo "smoke-test: FAILED" >&2
fi
exit "$failed"
