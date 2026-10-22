#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

mkdir -p "$REPO_ROOT/images"

BASE_URL="https://github.com/runout77/test_contrek/releases/download/test-assets-v1"

FILE="test_100k.png"
EXPECTED_SIZE=900175425
DEST="$REPO_ROOT/images/$FILE"

if [[ -f "$DEST" ]] && [[ $(stat -c %s "$DEST") -eq $EXPECTED_SIZE ]]; then
    echo "[OK] $FILE already present"
    exit 0
fi

echo "[DOWNLOAD] $FILE"

curl -fL --retry 3 \
    "$BASE_URL/$FILE" \
    -o "$DEST"

ACTUAL_SIZE=$(stat -c %s "$DEST")

if [[ "$ACTUAL_SIZE" -ne "$EXPECTED_SIZE" ]]; then
    echo "[ERROR] Invalid file size: $ACTUAL_SIZE (expected $EXPECTED_SIZE)" >&2
    exit 1
fi

echo "[OK] $FILE downloaded successfully"
