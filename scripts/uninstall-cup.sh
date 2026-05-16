#!/bin/sh

CUP_ROOT="$1"
SELF_PATH="$2"

sleep 1

if [ -z "$CUP_ROOT" ]; then
    exit 1
fi

rm -rf "$CUP_ROOT"

if [ -n "$SELF_PATH" ]; then
    rm -f "$SELF_PATH"
fi

exit 0