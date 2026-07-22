#!/usr/bin/env python3
import pathlib
import re
import sys


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    charsets = {}
    for line in (root / "build" / "Aliases").read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        canonical, mib, *aliases = line.split()
        charsets[canonical] = (mib, aliases)

    lines = [
        "/* Generated from NetSurf LibParserUtils build/Aliases. */",
        "static parserutils_charset_aliases_canon canonical_charset_names[] = {",
    ]
    aliases_by_name = {}
    unicode_mibs = []
    for index, canonical in enumerate(sorted(charsets)):
        mib, aliases = charsets[canonical]
        lines.append(f'\t{{ {mib}, {len(canonical)}, "{canonical}" }},')
        if re.match(r"^(ISO-10646-UCS-[24]|UTF-16|UTF-8$|UTF-32)", canonical):
            unicode_mibs.append(mib)
        for name in [canonical, *aliases]:
            normalized = re.sub("[^a-z0-9]", "", name.lower())
            aliases_by_name[normalized] = index
    lines += [
        "};",
        "",
        f"static const uint16_t charset_aliases_canon_count = {len(charsets)};",
        "",
        "typedef struct {",
        "\tuint16_t name_len;",
        "\tconst char *name;",
        "\tparserutils_charset_aliases_canon *canon;",
        "} parserutils_charset_aliases_alias;",
        "",
        "static parserutils_charset_aliases_alias charset_aliases[] = {",
    ]
    for name in sorted(aliases_by_name):
        lines.append(
            f'\t{{ {len(name)}, "{name}", '
            f'&canonical_charset_names[{aliases_by_name[name]}] }},'
        )
    expression = " || ".join(f"((x) == {mib})" for mib in unicode_mibs)
    lines += [
        "};",
        "",
        f"static const uint16_t charset_aliases_count = {len(aliases_by_name)};",
        "",
        f"#define MIBENUM_IS_UNICODE(x) ({expression})",
        "",
    ]
    target = root / "src" / "charset" / "aliases.inc"
    target.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
