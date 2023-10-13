#!/bin/bash

# Check the first argument provided to the script
if [ "$1" = "match" ]; then
    shift
    exec /Aureus/match "$@"
elif [ "$1" = "extract" ]; then
    shift
    exec /Aureus/extract "$@"
else
    echo "Please specify 'match' or 'extract' as the first argument."
    echo "Usage: $0 [match|extract] [additional arguments]"
    exit 1
fi
