#!/bin/bash

# Usage:
# - Run tests:
# # make check
# - Filter failures:
# # ci/ignore.sh < test/results/list

IGNORE_FILE="$(dirname "$0")/IGNORE"
if [[ ! -f "$IGNORE_FILE" ]]; then
	echo "ERROR: Skip file '$IGNORE_FILE' not found" >&2
	exit 2
fi

# Find all failed tests, sort the list, and filter out anything matching patterns in IGNORE_FILE:
# sed: ignore all comments and print non empty lines only
grep failed | sort | grep -v -f <(sed -n -e 's/\s*#.*$//' -e '/^.\+$/p' "$IGNORE_FILE")
