#!/bin/bash
set -e
. common.sh
declare -r FILE=a

main () {
	chdosattr -l $TEST_FILE_SIZE "$FILE"

	if ! assert_zeros "$FILE"
	then
		echo "OK: garbage read after extending VDL"
		return 0
	else
		echo "FAIL: NO garbage read after extending VDL"
		return 1
	fi
}

do_mount_and_push
main
