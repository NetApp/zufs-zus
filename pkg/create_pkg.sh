#!/bin/bash -e
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Package script
#
# Copyright (C) 2019 NetApp, Inc.  All rights reserved.

SCRIPT_PATH="$(readlink -f ${BASH_SOURCE[0]})"
SCRIPT_DIR=$(dirname $SCRIPT_PATH)
PKG_TYPE=${1:-rpm}
TMPDIR=$(mktemp -d)

${SCRIPT_DIR}/install.sh ${TMPDIR} ${PKG_TYPE}

ZUS_DIR=${SCRIPT_DIR}/..
cd ${ZUS_DIR}
GIT_HASH=$(git rev-parse HEAD)

DEPENDS="-d zufs-zuf -d lsof"
if [[ "${PKG_TYPE}" == "rpm" ]] ; then
	DEPENDS+=" -d libunwind -d libuuid -d procps-ng -d systemd"
elif [[ "${PKG_TYPE}" == "deb" ]] ; then
	DEPENDS+=" -d libunwind8 -d libuuid1 -d procps -d systemd"
fi

fpm -s dir -t ${PKG_TYPE} -n zufs-zus -v ${VER} -C ${TMPDIR} \
	--iteration ${BUILD_ID} --epoch 1 \
	--url "netapp.com" --license "GPL/BSD" --vendor "NetApp Inc." \
	--description "`printf "ZUS - Zero-copy User-mode Server\nID: ${GIT_HASH}"`" \
	${DEPENDS} --rpm-rpmbuild-define "_build_id_links none" \
	--before-remove pkg/pre_uninstall.sh \
	--after-remove pkg/post_uninstall.sh \
	--after-install pkg/post_install.sh .

rm -rf ${TMPDIR}

