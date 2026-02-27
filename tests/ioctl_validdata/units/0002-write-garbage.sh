#!/bin/bash
set -e
. common.sh
declare -r FILE=b

main () {
	local test_file_blocks

	do_mount_and_push

	chdosattr -l $TEST_FILE_SIZE "$FILE"

	if assert_zeros "$FILE"
	then
		echo "FAIL: NO garbage read immediately after extending VDL"
		return 1
	fi

	let 'test_file_blocks = TEST_FILE_SIZE / BLOCKSIZE'
	dd \
		status=none bs="$BLOCKSIZE" count="$test_file_blocks" \
		iflag=fullblock conv=nocreat,notrunc if=/dev/zero of="$FILE"

	qpopd
	do_umount

	do_mount_and_push

	if ! assert_zeros "$FILE"
	then
		echo "FAIL: garbage read writing zeros"
		return 1
	else
		echo "OK: no garbage read after writing zeros"
		return 0
	fi
}

main
