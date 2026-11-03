#!/bin/sh
# copies common/agrilink.h into every sketch, run it after changing it
cd "$(dirname "$0")"
for d in motion sensors audio display; do cp common/agrilink.h "$d/agrilink.h"; done
echo "agrilink.h synced"
