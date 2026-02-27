# 0xdeadbeef aligned to the minimum cluster size(4096 bytes)
declare -r BLOCKSIZE=4096
declare -r NB_BLOCKS=32768 # 128MB
declare -r TMPDIR="/tmp/exfatprogs-test-ioctl-validdata"
declare -r TEST_FILE="garage" # this is not a typo
declare -r TEST_FILE_SIZE=16777216 # 16MB

qpushd () {
	pushd "$@" > /dev/null
}

qpopd () {
	popd "$@" > /dev/null
}

# $@: additional args to mount command
do_mount_and_push () {
	mount $@ -t exfat "$TMPDIR/$TEST_FILE" "$TMPDIR/m"
	qpushd "$TMPDIR/m"
}

do_umount () {
	umount "$TMPDIR/m"
}

mtime_sleep () {
	# exfat has 10 ms mtime resolution
	sleep 0.01
}

get_isize () {
	stat -c '%s' "$1"
}

get_mtime () {
	stat -c '%y' "$1"
}

get_vdl () {
	local e
	local i=0
	local isize
	local vdl

	for e in `lsdosattr "$1"`
	do
		let 'i += 1'

		case "$i" in
		1) continue ;;
		2) isize="$e" ;;
		3) vdl="$e" ;;
		*) break ;;
		esac
	done

	if [ -z "$isize" ] || [ -z "$vdl" ]
	then
		return 1
	fi

	if [ "$vdl" == "=" ]
	then
		echo "$isize"
	else
		echo "$vdl"
	fi
	return 0
}

assert_empty () {
	if [ "$(get_isize "$1")" -ne 0 ]
	then
		echo "FAIL: isize of an empty file not zero"
		exit 1
	fi
	if [ "$(get_vdl "$1")" -ne 0 ]
	then
		echo "FAIL: vdl of an empty file not zero"
		exit 1
	fi
}

assert_zeros () {
	if xxd -ps "$@" | grep -qE '[^0]'
	then
		return 1
	else
		return 0
	fi
}

do_chdosattr () {
	chdosattr $@ 2> /dev/null
}

get_devoffile () {
	stat -c '%Hd:%Ld' "$1"
}

get_mpbydev () {
	while read line
	do
		if [ "$(echo "$line" | cut -d ' ' -f3)" == "$1" ]
		then
			echo "$line"
			return 0
		fi
	done < /proc/self/mountinfo

	return 1
}

get_clustersize () {
	local blkdev="$(get_mpbydev "$1" | cut -d ' ' -f 10)"
	local ei="$(dump.exfat "$blkdev")"
	local bps
	local css
	local ret

	[ -z "$blkdev" ] && return 1

	bps="$(echo -n "$ei" | sed -En 's/Bytes per Sector:([[:space:]]+)?([[:digit:]]+)/\2/p')"
	css="$(echo -n "$ei" | sed -En 's/Sectors per Cluster:([[:space:]]+)?([[:digit:]]+)/\2/p')"
	[ -z "$bps" ] || [ -z "$css" ] && return 1

	let 'ret = bps * css'
	echo "$ret"
}
