#!/bin/sh

CUP_ROOT="$1"
SELF_PATH="$2"

sleep 1

if [ -z "$CUP_ROOT" ]; then
    echo "Error: missing cup root."
    exit 1
fi

rm -rf "$CUP_ROOT"

if [ -n "$SELF_PATH" ]; then
    rm -f "$SELF_PATH"
fi

echo "cup has been uninstalled."
echo "Note: PATH entries were not removed."