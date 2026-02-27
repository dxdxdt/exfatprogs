#!/bin/bash
. common.sh
do_umount 2> /dev/null
rm -rf "${TMPDIR}"
