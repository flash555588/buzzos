#!/usr/bin/env python3
import pathlib
import sys


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    entities = []
    for line in (root / "build" / "Entities").read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        name, code, *_ = line.split()
        entities.append((name, code))

    nodes = []

    def insert(index, key, code):
        pivot = key[:1]
        tail = key[1:]
        if index is None:
            index = len(nodes)
            nodes.append({"pivot": pivot, "lt": None, "eq": None,
                          "gt": None, "value": None})
        node = nodes[index]
        if pivot < node["pivot"]:
            node["lt"] = insert(node["lt"], key, code)
        elif pivot == node["pivot"]:
            if not pivot:
                node["value"] = code
            elif not tail[:1]:
                node["value"] = code
                node["eq"] = insert(node["eq"], tail, code)
            else:
                node["eq"] = insert(node["eq"], tail, code)
        else:
            node["gt"] = insert(node["gt"], key, code)
        return index

    root_index = None
    for name, code in entities:
        root_index = insert(root_index, name, code)

    lines = [
        "/* Generated from NetSurf Hubbub build/Entities. */",
        "static hubbub_entity_node dict[] = {",
    ]
    for node in nodes:
        pivot = ord(node["pivot"]) if node["pivot"] else 0
        links = [node[name] if node[name] is not None else -1
                 for name in ("lt", "eq", "gt")]
        value = node["value"] if node["value"] is not None else "0"
        lines.append(
            f"\t{{ {pivot}, {links[0]}, {links[1]}, {links[2]}, {value} }},"
        )
    lines += ["};", "", f"static int32_t dict_root = {root_index};", ""]
    target = root / "src" / "tokeniser" / "entities.inc"
    target.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
