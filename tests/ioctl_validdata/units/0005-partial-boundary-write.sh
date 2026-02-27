#!/bin/bash
set -e
. common.sh
declare -r FILE=e

main () {
	local dev="$(get_devoffile "$FILE")"
	local csb="$(get_clustersize "$dev")"
	local ofs

	let 'ofs = csb - 1'

	# read the first cluster just to demonstrate the page in/out behaviour
	dd status=none bs="$csb" count=1 iflag=fullblock if="$FILE"

	chdosattr -l $TEST_FILE_SIZE "$FILE"

	# assert garbage data by reading the first two clusters
	if dd status=none bs="$csb" count=2 iflag=fullblock if="$FILE" | grep -q '55'
	then
		echo "FAIL: test marker read before testing"
		return 1
	fi

	# write 55's in the last and first bytes of two clusters
	echo '5555' -n | xxd -ps -r | dd status=none bs=1 seek="$ofs" count=2 iflag=fullblock conv=nocreat,notrunc of="$FILE"

	# remount to drop caches
	qpopd
	do_umount
	do_mount_and_push

	# read back, check data in direct and async mode
	local data="$(dd status=none bs=1 skip="$ofs" count=2 iflag=fullblock if="$FILE" | xxd -ps)"

	if [ "$data" != "5555" ]
	then
		echo "FAIL: expected 0x5555, got 0x$data"
		return 1
	fi

	echo "OK"
	dd status=none bs="$csb" count=3 iflag=fullblock if="$FILE" |
		hexdump -C >&2 | grep --color=always -e ' 55' -e '^'
}

do_mount_and_push
main
