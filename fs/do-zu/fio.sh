#!/bin/bash -e

DIR=/mnt/m1fs
#DIR=/home/nave/fuse_tutorial/fuse-tutorial-2016-03-25/example/mountdir
SIZE=2147483648
NAME=fio_test
RES=fio.res

OUTPUT=FIO_TMP_OUT

FIO=/opt/fio/fio
# 1. JOBS 2. BLOCK-SIZE
do_fio()
{
	local JOBS=$1
	local BS=$2

	$FIO \
		--numjobs=$JOBS --bs=$BS \
		--size=$((SIZE / JOBS)) --directory=$DIR --name=$NAME \
		--rw=randwrite --sync=1 --direct=1 \
		--ioengine=psync --runtime=15 --thinktime=0 \
		--norandommap --fallocate=none --group_reporting --minimal \
		--output=${OUTPUT} \
	;

	# These are for write see http://www.andypeace.com/fio_minimal.html
	fio_total_io=$(awk -F";" '{print $47}' ${OUTPUT})
	fio_throughput=$(awk -F";" '{print $48}' ${OUTPUT})
	fio_iops=$(awk -F";" '{print $49}' ${OUTPUT})
	fio_lat=$(awk -F";" '{print $81}' ${OUTPUT})

	echo "fio_total_io=$fio_total_io fio_throughput=$fio_throughput fio_iops=$fio_iops fio_lat=$fio_lat"
}

RM=/usr/bin/rm

fio_4k_tX()
{
	local n=$1

	echo -n "[fio_4k_t$n] "		>> $RES
	do_fio $n 4096			>> $RES
	$RM -f $DIR/*
}

# not used
fio_mt_64()
{
	echo -n "[fio_mt_64] "		>> $RES
	do_fio `nproc` 64		>> $RES
	$RM -f $DIR/*
}

if_lt_fio_4k_tX()
{
	if [ $1 -lt $2 ]; then
		fio_4k_tX $1
	fi
}

fio_all()
{
	local nproc=`nproc`

	echo "========= $(date) $1 nproc=$nproc ======"	>> $RES
	fio_4k_tX 1
	fio_4k_tX 2
	fio_4k_tX 4
	if_lt_fio_4k_tX 8  $nproc
	if_lt_fio_4k_tX 12 $nproc
	if_lt_fio_4k_tX 18 $nproc
	if_lt_fio_4k_tX 24 $nproc
	if_lt_fio_4k_tX 36 $nproc

	fio_4k_tX $nproc
	cat $RES
}

fio_all $@
