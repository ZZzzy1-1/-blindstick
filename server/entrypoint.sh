#!/bin/bash
# Set PATH to remove any injected paths
export PATH="/usr/local/bin:/usr/bin:/bin:$PATH"
cd "$(dirname "$0")"
exec python proxy_server.py
