#!/bin/bash

# Usage: ./writer.sh <file_path> <string_to_write>

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <file_path> <string_to_write>"
    exit 1
fi

FILE_PATH="$1"
WRITE_STR="$2"

mkdir -p $(dirname "$FILE_PATH")

echo "$WRITE_STR" > "$FILE_PATH"