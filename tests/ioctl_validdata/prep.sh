#!/bin/bash
set -e
. common.sh
declare -r GARBAGE_FILE="garbage_block"

spew_garbage () {
	while echo -n 'deadbeef' | xxd -ps -r
	do
		:
	done
}

repeat_file () {
	while cat "$@"
	do
		:
	done
}

rm -rf "$TMPDIR"
mkdir -p "$TMPDIR/m"

qpushd "$TMPDIR"

# make a garbage block
spew_garbage | dd status=none bs="$BLOCKSIZE" count=1 iflag=fullblock of="$GARBAGE_FILE"

# fill the garages up with garbage
repeat_file "$GARBAGE_FILE" | \
	dd status=none bs="$BLOCKSIZE" count="$NB_BLOCKS" iflag=fullblock of="$TEST_FILE"
mkfs.exfat "$TEST_FILE"

do_mount_and_push
# create new entries and truncate them up: same effect as fallocate() mode 0
truncate -s "$TEST_FILE_SIZE" a b c d e
qpopd
do_umount

qpopd
