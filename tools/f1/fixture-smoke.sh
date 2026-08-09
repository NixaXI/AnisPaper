#!/bin/sh
set -eu
if python3 "$(dirname "$0")/../../tests/f1_integration.py" "$1"; then exit 0; else status=$?; fi
[ "$status" -eq 77 ]
