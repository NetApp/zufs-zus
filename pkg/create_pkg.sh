#!/bin/bash -e

SCRIPT_PATH="$(readlink -f ${BASH_SOURCE[0]})"
SCRIPT_DIR=$(dirname $SCRIPT_PATH)

TMPDIR=$(mktemp -d)
${SCRIPT_DIR}/install.sh ${TMPDIR}

ZUS_DIR=${SCRIPT_DIR}/..
cd ${ZUS_DIR}
GIT_HASH=$(git rev-parse HEAD)

fpm -s dir -t rpm -n zufs-zus -v ${VER} -C ${TMPDIR} \
	--iteration ${BUILD_ID} --epoch 1 \
	--url "netapp.com" --license "GPL/BSD" --vendor "NetApp Inc." \
	--description "`printf "ZUS - Zero-copy User-mode Server\nID: ${GIT_HASH}"`" \
	-d libunwind -d libuuid -d lsof -d zufs-zuf \
	--rpm-rpmbuild-define "_build_id_links none" \
	--before-remove pkg/pre_uninstall.sh \
	--after-remove pkg/post_uninstall.sh \
	--after-install pkg/post_install.sh .

rm -rf ${TMPDIR}

