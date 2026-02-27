#!/bin/bash
set -e -o pipefail
. common.sh
declare -r FILE=d

main () {
	# This is quite sketchy because we have no control over the system
	# memory situation!

	do_mount_and_push -o sync

	if ! assert_zeros "$FILE"
	then
		echo "FAIL: garbage read before extending VDL"
		return 1
	fi

	# now set the VDL
	chdosattr -l $TEST_FILE_SIZE "$FILE"

	# bypass cache
	if !(dd status=none iflag=fullblock,direct if="$FILE" | assert_zeros -)
	then
		echo "OK: garbage read"
	else
		echo "FAIL: no garbage read"
		return 1
	fi
}

main
