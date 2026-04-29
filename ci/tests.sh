#!/bin/bash

# Usage:
# # ci/tests.sh . < test/results/list

FILTER_FILE="$(dirname "$0")/ONLY"
if [[ ! -f "$FILTER_FILE" ]]; then
	echo "ERROR: Filter file '$FILTER_FILE' not found" >&2
	exit 2
fi

SKIP_FILE="$(dirname "$0")/SKIP"
if [[ ! -f "$SKIP_FILE" ]]; then
	echo "ERROR: Skip file '$SKIP_FILE' not found" >&2
	exit 2
fi

TEST_DIR="${1:-"."}/test"
if [[ ! -d "$TEST_DIR" ]]; then
	echo "ERROR: Test directory '$TEST_DIR' not found" >&2
	exit 2
fi

# Find all tests, skip ones matching pattern in SKIP_FILE and sort the result:
# sed: ignore all comments and print non empty lines only
(cd "$TEST_DIR" && find . -path '*.sh' | grep -v '\(/lib/\)') | sed 's|^\./||' | grep -f <(sed -n -e 's/\s*#.*$//' -e '/^.\+$/p' "$FILTER_FILE") | grep -v -f <(sed -n -e 's/\s*#.*$//' -e '/^.\+$/p' "$SKIP_FILE") | sort
