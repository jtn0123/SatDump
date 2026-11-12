#!/bin/bash
# Linux builds use Debian's muparser (cmake/fork/system_parsers.cmake): the installed
# libsatdump_core.so needs libmuparser.so.2 and carries none of muparser's own code.
. "$(dirname "$0")/common.sh"

CORE=$(find "$PREFIX" -name 'libsatdump_core.so' -print -quit)
[ -n "$CORE" ] || { echo "FAIL no libsatdump_core.so under $PREFIX"; exit 1; }
readelf -d "$CORE" > "$WORK/dynamic.txt"
nm -D -C --defined-only "$CORE" > "$WORK/defined.txt"
nm -D -C --undefined-only "$CORE" > "$WORK/undefined.txt"

check needs-libmuparser grep -q 'NEEDED.*\[libmuparser\.so\.2\]' "$WORK/dynamic.txt"

# muparser's compiled code (its .cpp files) comes from libmuparser, so these are undefined here
for f in 'mu::Parser::Parser()' 'mu::ParserBase::SetExpr(' 'mu::ParserBase::DefineVar(' \
    'mu::ParserBase::Eval(int&) const' 'mu::ParserError::GetMsg'; do
    check "from-libmuparser ${f%%(*}" grep -qF " U $f" "$WORK/undefined.txt"
done

# No strong definition of anything in mu:: (nm types T, D, B, R: code or data from muparser's
# .cpp files). Weak ones (W, V) may stay: they are inline functions and type information from
# the headers, which the compiler puts in every library that uses them when it does not inline
# them (sanitizer and debug builds do this; -O3 inlines nearly all)
check no-muparser-code-inside test "$(grep -cE '^[0-9a-f]+ [TtDdBbRr] mu::' "$WORK/defined.txt")" = 0

exit $FAILED
