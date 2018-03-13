#!/bin/bash -e

do_fio()
{
	local BS=$1
	local THREAD=$2
	local ITERS=3

	fio_total_io=0
	fio_throughput=0
	fio_iops=0
	fio_lat=0
	res=()

	stat_b=($(cat $STAT))
	for ((i=0; i < $ITERS; i++)); do
		drop_caches
		$FIO	--sync=1 \
			--direct=0 \
			--filename=${FILE} \
			--invalidate=1 \
			--rw=randread \
			--bs=${BS} \
			--randseed=123456 \
			--allrandrepeat=1 \
			--norandommap \
			--ioengine=psync \
			--size="$(echo ${SIZE} / ${THREAD} | bc)" \
			--filesize=${FILESIZE} \
			--numjobs=${threads} \
			--group_reporting \
			--minimal \
			--output=${OUTPUT} \
			--name=fio_test_pxs380

		fio_total_io=$(echo $fio_total_io + $(awk -F";" '{print $6}' ${OUTPUT}) | bc -l)
		fio_throughput=$(echo $fio_throughput + $(awk -F";" '{print $7}' ${OUTPUT}) | bc -l)
		fio_iops=$(echo $fio_iops + $(awk -F";" '{print $8}' ${OUTPUT}) | bc -l)
		fio_lat=$(echo $fio_lat + $(awk -F";" '{print $40}' ${OUTPUT}) | bc -l)
	done
	stat_a=($(cat $STAT))

	fio_total_io=$(echo "scale=3;$fio_total_io / ${ITERS} / 1024" | bc -l)
	fio_throughput=$(echo "scale=3;$fio_throughput / ${ITERS} / 1024" | bc -l)
	fio_iops=$(echo "scale=3;$fio_iops / ${ITERS}" | bc -l)
	fio_lat=$(echo "scale=3;$fio_lat / ${ITERS}" | bc -l)

	echo -ne "${fio_total_io}\t${fio_throughput}\t${fio_iops}\t${fio_lat}\t"
	for i in $(seq 0 10); do
		res[i]=$(echo "scale=3;(${stat_a[i]} - ${stat_b[i]}) / ${ITERS}" | bc -l)
		echo -ne "${res[i]}\t"
	done
}

usage()
{
	[ $1 -ne 0 ] && exec >&2

	cat << EOF
$PROGRAM - $DESCRIPTION
usage:
  $PROGRAM $OPTIONS

options:
  -h    help: show this help message and exit.
  -f	fallocate: whether to fallocate the file or not (default=false).
  -F	file-name: name of file to run performance checks on.
  -s	file-size: size of file to run performance checks on
	(default=filesystem size).

EOF

    exit $1
}

check_perf()
{
	PROGRAM=$(basename "$0")
	DESCRIPTION="Check performance of a partition"
	OPTIONS="[-hf] [-F <file-name>] [-s <filesize>]"

	FILE=""
	FILESIZE=""
	FALLOCATE=false

	while getopts hfF:s: option; do
		case $option in
			h) usage 0 ;;
			f) FALLOCATE=true ;;
			F) FILE="$OPTARG" ;;
			s) FILESIZE="$OPTARG" ;;
			\?) usage 255 ;;
		esac
	done
	shift $((OPTIND - 1))

	OUTPUT=/tmp/fio_output
	DEV_NAME=$(echo ${T2_DEV0/\/dev\//})

	#must be run as root
	if [[ $UID -ne 0 ]]; then
		echo "must be run as root"
		exit 1
	fi

	if [[ $DEV_NAME =~ .*p[0-9] ]]; then
		_DEV=${DEV_NAME%p[0-9]}
		DEV_NAME="$_DEV/$DEV_NAME"
	fi
	STAT="/sys/block/$DEV_NAME/stat"

	if ! [ -f "$FILE" ]; then
		touch $FILE
		if [[ -z $FILESIZE ]]; then
			FILESIZE=$(($(df $FILE | sed 1d | awk '{print $4}') * 1024))
		fi
		if [[ $FALLOCATE == true ]]; then
			fallocate -l $FILESIZE $FILE || true
		fi
		$FIO --filename=$FILE --size=$FILESIZE --fallocate=none --numjobs=8 --rw=write --group_reporting --name=mizzi --bs=4k > /dev/null || true
	fi

	drop_caches
	FILE_STAT=($(stat --format="%b %B" $FILE))
	FILESIZE=$((${FILE_STAT[0]} * ${FILE_STAT[1]}))
	SIZE=$(($FILESIZE / 8))

	THREADS="${THREADS:-1 2 4 8 16}"
	BLOCKSIZE="${BLOCKSIZE:-4096 8192 16384 32768 65536 131072 262144 524288 1048576}"

	if [[ $FILE =~ .*m1fs.* ]]; then
		PREFIX="m1fs"
	else
		PREFIX="ext4"
	fi

	for threads in $THREADS; do
		_RES_FILE="$RESULTS_DIR/"$PREFIX"_"${threads}"t.txt"

		echo -ne "threads\tblksize\tfio_total_io[MB]\tfio_throughput[MB/s]\tfio_iops\t" > $_RES_FILE
		echo -ne "fio_latency[us]\tread_ios\tread_merges\tread_sectores\tread_ticks\t" >> $_RES_FILE
		echo -ne "write_ios\twrite_merges\twrite_sectores\twrite_ticks\tin_flight\t" >> $_RES_FILE
		echo -e "io_ticks\ttime_in_queue" >> $_RES_FILE
		for bs in $BLOCKSIZE; do
			echo -e "${threads}\t${bs}\t$(do_fio ${bs} ${threads})" >> $_RES_FILE
		done
	done
}
