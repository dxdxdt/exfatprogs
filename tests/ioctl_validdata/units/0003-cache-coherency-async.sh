#!/bin/bash
set -e -o pipefail
. common.sh
declare -r FILE=c

main () {
	# This is quite sketchy because we have no control over the system
	# memory situation!

	do_mount_and_push -o async

	# first, read them all so that the blocks are cached
	if ! assert_zeros "$FILE"
	then
		echo "FAIL: garbage read before extending VDL"
		return 1
	fi

	# now set the VDL
	chdosattr -l $TEST_FILE_SIZE "$FILE"

	if ! assert_zeros "$FILE"
	then
		echo "COHERENT: zeros read first, then garbage read after extending VDL"
	else
		echo "INCOHERENT: no garbage read after reading first and extending VDL"
	fi
}

main
