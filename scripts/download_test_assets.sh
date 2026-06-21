#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

mkdir -p "$REPO_ROOT/images"

BASE_URL="https://github.com/runout77/test_contrek/releases/download/test-assets-v1"

FILES=(
    "test_81920x81920.png"
    "high_complexity_81920x81920.png"
)

for FILE in "${FILES[@]}"; do

    if [ -f "$REPO_ROOT/images/$FILE" ]; then
        echo "[OK] $FILE already present"
        continue
    fi

    echo "[DOWNLOAD] $FILE"

    curl -L --fail \
        "$BASE_URL/$FILE" \
        -o "$REPO_ROOT/images/$FILE"

done