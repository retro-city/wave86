#!/bin/sh
# WAVE86 server for macOS and Linux: README.txt says what it needs. The
# Python in .venv is used when there is one (pip install -r
# requirements.txt), the system's python3 otherwise. Anything after
# run-server.sh goes to the server: ./run-server.sh --port 8090
cd "$(dirname "$0")" || exit 1
PY=python3
[ -x .venv/bin/python ] && PY=.venv/bin/python
exec "$PY" tools/waveserve.py "$@"
