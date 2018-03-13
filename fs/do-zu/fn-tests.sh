#!/bin/bash

CHECK_GIT_NUM_PRE_FILES=2000
touch_pre_files()
{
	local dir="$1"
	local j

	mkdir -p $dir
	for ((j=0;j<$CHECK_GIT_NUM_PRE_FILES;j++)); do
		touch $dir/$j

		local a=$(( j % 10 ));
		if [[ $dir == 5 && 0 == $a ]]; then
			echo -ne "file $j" \\r;
		fi
	done
}

do_check_git()
{
	orig_dir=`pwd`
	DO_M1_DIR=".build_dom1"
	JOBS=${JOBS:-14}

	is_mounted $MOUNT_POINT
	if [[ $? == 0 ]]; then
		echo "please run: $0 again"
		return 1
	fi

	case $1 in
	ALL)
		echo ALL ...
		unset CHK_GIT_FAST
		;;
	FAST)
		echo FAST ...
		CHK_GIT_FAST="yes"
		;;
	*)
		echo "DEF CHK_GIT_FAST=$CHK_GIT_FAST"
		;;
	esac

	cd $MOUNT_POINT

	if [[ -z $CHK_GIT_FAST ]]; then
		# rm old tree
		rm -rf * | return 3

		echo -n "create $CHECK_GIT_NUM_PRE_FILES * 100 files @ "; pwd

		local i
		for ((i=0;i<100;i++)); do
			touch_pre_files $i &
		done
		wait
	fi

	df -hi ./

	if [[ -z $CHK_GIT_FAST || ! -e $GIT_REPO_NAME ]]; then
		# git clone
		echo -n "cloning $GIT_REPO_DIR/$GIT_REPO_NAME @ "; pwd
		git clone $GIT_REPO_DIR/$GIT_REPO_NAME | return 4
	fi

	cd $GIT_REPO_NAME
	git status | return 6

	if [[ -z $CHK_GIT_FAST ]]; then

		cd $orig_dir
		# umount/mount
		foo_umount | return 7
		foo_mount | return 8

		# git status
		cd $MOUNT_POINT/$GIT_REPO_NAME
		echo -n "check status @ "; pwd
		git status | return 10
		ALL_CONFIG=allmodconfig
	else
		ALL_CONFIG=defconfig
		make O=$DO_M1_DIR clean
	fi

	mkdir -p $DO_M1_DIR

	# make all/defconfig
	cmd="make O=$DO_M1_DIR $ALL_CONFIG"
	echo -n "$cmd @ "; pwd
	eval $cmd || return 11

	# make -j X
	cmd="make O=$DO_M1_DIR -j $JOBS"
	echo -n "$cmd @ "; pwd
	eval $cmd || return 12

	cd $orig_dir
	return 0;
}

create_files()
{
	local dir=$1
	local files=$2
	local prefix=$3
	local j

	mkdir -p $dir
	for ((j=0;j<$files;j++)); do
		echo -n > $dir/${prefix}_${j}
	done
}

do_check_mktree()
{
	is_mounted $MOUNT_POINT
	if [[ $? == 0 ]]; then
		echo "please run: $0 again"
		return 1
	fi

	cd $MOUNT_POINT
	# rm old tree
	rm -rf * | return 3

	free_inode=`df -i $MOUNT_POINT | tail -1 | awk '{print $4}'`
	echo "free-inodes count $free_inode"
	files_per_dir=1000
	loop=$((free_inode / files_per_dir + 1))
	mkdir -p $MOUNT_POINT/testdir

	echo "Create $((loop * files_per_dir)) files in $MOUNT_POINT/testdir"
	for ((i=0;i<$loop;i++)); do
		create_files $MOUNT_POINT/testdir $files_per_dir $i 2>&1 &
	done
	wait
}

do_xfstest()
{
	if [ -n "$1" ]; then
		XFS_TEST=$1
		shift
	fi

	local MOUNT_DEV="$DEV"
	local MOUNT_DEV2="$DEV2"
	export FSTYP=zuf
	export ZUF_FSTYP=$ZUF_FSTYP

	# 1st m1fs $DEF_MOUNT_OPT must end with a ','
	TEST_FS_MOUNT_OPTS="-o $DEF_MOUNT_OPT"
	# 2nd m1fs
	MKFS_OPTIONS="$DEF_MKFS_OPT"
	MOUNT_OPTIONS="$DEF_MOUNT_OPT"
	if [ -n "$EXTRA_SCRATCH_DEVS" ]; then
		MKFS_OPTIONS="$MKFS_OPTIONS $EXTRA_SCRATCH_DEVS"
	fi
	if [ -n "$T2_DEV0" ]; then
		MOUNT_DEV="$T2_DEV0"
		# 2nd m1fs
		MKFS_OPTIONS="$MKFS_OPTIONS --t1=$DEV2"
		MOUNT_DEV2="$T2_DEV1"
	fi
	if [[ ! -z $MOUNT_OPTIONS ]]; then
		MOUNT_OPTIONS="-o $MOUNT_OPTIONS"
	fi
	if lsmod | grep -q m1fs_target; then
		[[ -n "$TARGET_IP" ]] && MOUNT_OPTIONS+=",rmem=$TARGET_IP"
	fi

	export TEST_FS_MOUNT_OPTS
	export MOUNT_OPTIONS
	export MKFS_OPTIONS

	export TEST_DEV=$MOUNT_DEV
	export TEST_DIR=$MOUNT_POINT

	export SCRATCH_DEV=$MOUNT_DEV2
	export SCRATCH_MNT=$MOUNT_SCRATCH

	mkdir -p $SCRATCH_MNT

	cd /opt/xfstests
	case $XFS_TEST in
	quick)
		./check -g quick ;;
	all)
		./check ;;
	raw)
		tests/$@ ;;
	list)
		./check $@ ;;
	single)
		./check generic/$@ ;;
	group)
		./check -g $@ ;;
	auto)
		./check -g auto $@ ;;
	git)
		do_check_git $@ ;;
	zzzzz)
		sleep $2 ;;
	*)
		./check generic/$XFS_TEST ;;
	esac
}

LOOP_STOP=/tmp/stop____xfstest_loop

__the_loop_xfstest_loop()
{
	local loop=$1
	shift

	rm -f $LOOP_STOP

	for (( c=1; c<=$loop; c++ ))
	do
		echo "============== loop $c ==============="
		for t in $@;
		do
			if [ -e $LOOP_STOP ]; then
				rm -f $LOOP_STOP
				echo "----- STOP THE LOOP -----"
				return
			fi
			echo "----- do_xfstest $t -----"
			do_xfstest $t
		done
	done
}

# eg: /do-m1 loop-xfstest 17 029 224 010 315 299 269 010 083 074 263 310 129
#                         ^ 17 times this list
do_loop_xfstest()
{
	local loop=$1
	shift

	case $1 in
	stop)
		echo "loop_xfstest: please stop next ..."
		touch $LOOP_STOP
		;;
	list)
		# don't shift let do_xfstest see the "list"
		for (( c=1; c<=$loop; c++ ))
		do
			echo "============== loop $c ==============="
			do_xfstest $@
		done
		;;
	magic)
		magic_list="029 224 320 256 010 315 299 269 010 083 074 263 310 129 011 320"
		echo "loop-xfstest magic-list $magic_list"
		__the_loop_xfstest_loop $loop $magic_list
		;;
	*)
		# One by one random order
		__the_loop_xfstest_loop $loop $@
		;;
	esac
}

do_ptk_t1()
{
	local JSON=/opt/ptk_tests/overnight_functionality.json
	[[ "$1" =~ ".json" ]] && JSON=$1
	# overnight_functionality
	#t1
	cd /opt/ptk
	./ptk.py -f m1fs 						\
		-c $JSON		\
		-t $DEV   -o  $DEF_MOUNT_OPT				\
		-t2 $DEV2 -o2 $DEF_MOUNT_OPT
}
do_ptk_t2()
{
	local JSON=/opt/ptk_tests/overnight_functionality_t2.json
	[[ "$1" =~ ".json" ]] && JSON=$1
	# with t2
	cd /opt/ptk
	./ptk.py -f m1fs 						  \
		-c $JSON	  \
		-t $DEV   -o  $DEF_MOUNT_OPT"t2=$T2_DEV0" -k  t2=$T2_DEV0 \
		-t2 $DEV2 -o2 $DEF_MOUNT_OPT"t2=$T2_DEV1" -k2 t2=$T2_DEV1
}
do_ptk()
{
	if [ -n $T2_DEV0 ]; then
		do_ptk_t2 $@;
	else
		do_ptk_t1 $@;
	fi
}

do_ptk_single()
{
	#single xfs test
	cd /opt/ptk
	./ptk.py -f m1fs \
		-c /opt/ptk_tests/single_xfs_test_jenkins.json  \
		--override RunXfsTest.xfs_test="\"generic/$1\"" \
		-t $DEV   -o  $DEF_MOUNT_OPT \
		-t2 $DEV2 -o2 $DEF_MOUNT_OPT
}

do_ptk_write_read_verify()
{
	# with t2
	cd /opt/ptk
	./ptk.py -f m1fs 						  \
		-c /opt/ptk_tests/jira_bugs/pxs_/write_fillmd_read_verify.json\
		-t $DEV   -o  $DEF_MOUNT_OPT"t2=$T2_DEV0" -k  t2=$T2_DEV0 \
		-t2 $DEV2 -o2 $DEF_MOUNT_OPT"t2=$T2_DEV1" -k2 t2=$T2_DEV1
}

do_xfs_again()
{
	echo "TEST_DEV=$T2_DEV0 MOUNT_POINT=$MOUNT_POINT SCRATCH_DEV=$T2_DEV1 MOUNT_SCRATCH=$MOUNT_SCRATCH"
	umount "/mnt/m1fs"
	mkfs.xfs -f $T2_DEV0
	mount $T2_DEV0 $MOUNT_POINT
}

do_xfs_xfstest()
{
	if [ -n "$1" ]; then
		XFS_TEST=$1
	fi

	export FSTYP=xfs

	# 1st xfs (must be already mounted use "do-m1 xfs-again")
	export TEST_FS_MOUNT_OPTS="$T2_DEV0"
	export TEST_DEV=$T2_DEV0
	export TEST_DIR=$MOUNT_POINT

	# 2nd xfs
	export MKFS_OPTIONS=""
	export MOUNT_OPTIONS=""
	export SCRATCH_DEV=$T2_DEV1
	export SCRATCH_MNT=$MOUNT_SCRATCH

	cd /opt/xfstests
	case $XFS_TEST in
	quick)
		./check -g quick ;;
	all)
		./check ;;
	raw)
		tests/$2 ;;
	list)
		shift;
		./check $@ ;;
	single)
		./check generic/$2 ;;
	group)
		./check -g $2 ;;
	auto)
		./check -g auto ;;
	*)
		./check generic/$XFS_TEST ;;
	esac
}

do_perf()
{
	FIO_JOB="$base_dir/perf.sh"
	FIO="/opt/fio/fio"
	PWDIR=`pwd`
	FULL=""
	if [[ "$1" == "full" ]]; then
		FULL="$1"
		shift
	fi

	cd $GIT_REPO_DIR/$GIT_REPO_NAME
	GIT=$(basename $(git describe --all --long))
	RES_DIR_NAME="results/`date --iso-8601=minutes`-$GIT-`hostname`"
	RESULTS_DIR="$base_dir/$RES_DIR_NAME"
	FILE="$MOUNT_POINT/mizzi-do-perf"
	cd $PWDIR

	$FIO -v > /dev/null || return 1
	mkdir -p $RESULTS_DIR
	test -w $RESULTS_DIR
	NONWRITEABLE=$?

	if [ $NONWRITEABLE ]; then
		RESULTS_DIR="$PWDIR/$RES_DIR_NAME"
		mkdir -p $RESULTS_DIR && test -w $RESULTS_DIR
	fi

	is_mounted $MOUNT_POINT && return 3

	echo "checking m1fs performance..."
	source $FIO_JOB
	check_perf -F $FILE $@ || return 6
	if [[ $NONWRITEABLE && ! -z $RES_HOST ]]; then
		scp -r $RESULTS_DIR $RES_HOST:$base_dir/results/
	fi
	echo "see $RESULTS_DIR"

	if [[ -z $FULL ]]; then
		return 0
	fi

	foo_umount > /dev/null
	EXT4_MNT_POINT="/mnt/ext4"
	FILE="$EXT4_MNT_POINT/mizzi-do-perf"

	mkdir -p $EXT4_MNT_POINT || return 7
	is_mounted $EXT4_MNT_POINT || umount $EXT4_MNT_POINT
	echo -ne "creating ext4 filesystem...\t"
	mkfs.ext4 $T2_DEV0
	mount $T2_DEV0 $EXT4_MNT_POINT
	echo "checking ext4 performance..."
	check_perf -F $FILE $@
	if [[ $NONWRITEABLE && ! -z $RES_HOST ]]; then
		scp -r $RESULTS_DIR $RES_HOST:$base_dir/results/
	fi
	umount $EXT4_MNT_POINT || return 8
	echo "see $RESULTS_DIR/ext4_*.txt"
}


kmg_to_num()
{
         local STR=$1
         [[ -z $STR ]] && return
         POS=$(( ${#STR}  - 1 ))
         NUM=${STR:0:$POS}
         case ${STR:$POS:1} in
                 G|g)
                         NUM=$(( NUM * 1024 ))
                         ;&
                 M|m)
                         NUM=$(( NUM * 1024 ))
                         ;&
                 K|k)
                         NUM=$(( NUM * 1024 ))
                         ;;
                 *)
                         NUM=$STR
         esac
         echo $NUM
}

num_to_kmg()
{
         local BYTES=$1
         if ! [[ $BYTES =~ ^-?[0-9]+$ ]] ; then
                 echo $BYTES
                 return
         fi

         local SIGNES=("" K M G)
         for (( I = 0; I < 4; ++I ))
         do
                 (( BYTES & 1023 )) && break
                 BYTES=$(( BYTES >> 10 ))
         done
         echo $BYTES${SIGNES[$I]}
}

my_dd()
{
	local cmd=$1
	local bs=$2
	local count=$3
	local file=$4
	shift; shift; shift; shift;

	if [ "$file" == "" ]; then
		file=dd-$bs-$count.dd
	fi

	case $cmd in
	a)
		local bs_d=$(kmg_to_num $bs)
		local cn_d=$(kmg_to_num $count)
		local L=$(( bs_d * cn_d ))
		L=$(num_to_kmg $L)
 		echo "fallocate -l $L $MOUNT_POINT/$file"
 		fallocate -l $L $MOUNT_POINT/$file $@
		;;
	w)
		echo "write $MOUNT_POINT/$file bs=$bs count=$count $@"
		dd if=/dev/zero of=$MOUNT_POINT/$file conv=notrunc \
			bs=$bs count=$count $@
		;;
	r)
		echo "read $MOUNT_POINT/$file bs=$bs count=$count"
		dd of=/dev/null if=$MOUNT_POINT/$file conv=notrunc \
			bs=$bs count=$count $@
		;;
	rm)
		rm -rfv $MOUNT_POINT/*
		;;
	*)
		echo "do-m1 a|w|r block_size count file_name_at $MOUNT_POINT"
	esac
}

do_git_compile()
{
	is_mounted $MOUNT_POINT
	if [[ $? == 0 ]]; then
		echo "please run: $0 again"
		return 1
	fi

	cd /opt/m1fstests
	./git_compile/git_compile.sh -n 3 -c 5 -l 1 -d $MOUNT_POINT
}
