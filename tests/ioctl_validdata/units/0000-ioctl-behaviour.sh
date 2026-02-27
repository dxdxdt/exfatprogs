#!/bin/bash
set -e -o pipefail
. common.sh
declare -r FILE="behaviour"

main () {
	local mtime

	do_mount_and_push

	truncate -s 0 "$FILE"
	assert_empty "$FILE"

	mtime="$(get_mtime "$FILE")"

	if ! do_chdosattr -l 0 "$FILE" || [ "$(get_vdl "$FILE")" != "0" ]
	then
		echo "FAIL: to set VDL to 0"
		return 1
	fi
	# mtime shouldn't change for no-op
	if [ "$mtime" != "$(get_mtime "$FILE")" ]
	then
		echo "FAIL: mtime changed after no-op ioctl"
		return 1
	fi

	mtime_sleep

	if do_chdosattr -l 1 "$FILE" || [ "$mtime" != "$(get_mtime "$FILE")" ]
	then
		echo "FAIL: setting VDL past isize successful"
		return 1
	fi

	fallocate -l 1 "$FILE"
	mtime_sleep

	mtime="$(get_mtime "$FILE")"
	do_chdosattr -l 1 "$FILE"
	mtime_sleep
	if [ "$(get_vdl "$FILE")" != "1" ]
	then
		echo "FAIL: to set VDL to 1"
		return 1
	fi
	if [ "$mtime" == "$(get_mtime "$FILE")" ]
	then
		echo "FAIL: mtime didn't change after successful VDL change"
		return 1
	fi
	mtime="$(get_mtime "$FILE")"
	mtime_sleep

	if do_chdosattr -l 0 "$FILE" ||
		[ "$mtime" != "$(get_mtime "$FILE")" ] ||
		[ "$(get_vdl "$FILE")" == "0" ]
	then
		echo "FAIL: setting VDL less than isize successful"
		return 1
	fi

	echo "OK"
}

main
