#!/usr/bin/env python3
"""Fail if click-review --json stdout contains any error or warn entries."""
from __future__ import annotations

import json
import sys


def main() -> int:
    data = json.load(sys.stdin)
    bad: list[str] = []
    for group, group_data in data.items():
        for kind in ("error", "warn"):
            for name, info in (group_data.get(kind) or {}).items():
                text = (info or {}).get("text", "")
                bad.append(f"{kind}: {group}:{name}: {text}")
    if bad:
        print("\n".join(bad))
        return 1
    print("clean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
