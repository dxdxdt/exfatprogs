#!/bin/bash
. common.sh

./cleanup.sh
./prep.sh

for unit in ./units/*
do
	echo -n "$(basename "$unit"): "
	do_umount 2> /dev/null
	"$unit"
done

./cleanup.sh
