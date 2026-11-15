#!/usr/bin/env python3
"""meta.py: tiny JSON key/value files (snapshot metadata, run status).

  meta.py set FILE key=value [key=value ...]   create or update (numbers stay numbers)
  meta.py get FILE key [default]               print one value (default, or empty, if missing)
"""
import json
import os
import sys


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def conv(v):
    for t in (int, float):
        try:
            return t(v)
        except ValueError:
            pass
    return v


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, path = sys.argv[1], sys.argv[2]
    d = load(path)
    if cmd == "set":
        for kv in sys.argv[3:]:
            k, _, v = kv.partition("=")
            d[k] = conv(v)
        tmp = path + ".tmp"
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(tmp, "w") as f:
            json.dump(d, f, indent=1, sort_keys=True)
        os.replace(tmp, path)
    elif cmd == "get":
        default = sys.argv[4] if len(sys.argv) > 4 else ""
        print(d.get(sys.argv[3], default))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
