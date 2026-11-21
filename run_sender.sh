#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ ! -x "$ROOT/.venv/bin/python" ]; then
    printf '%s\n' 'Python project environment missing. From repository root:' 'python3 -m venv .venv' '.venv/bin/python -m pip install -r requirements.txt' >&2
    exit 1
fi
exec "$ROOT/.venv/bin/python" "$ROOT/tools/workflow.py" "$@"
