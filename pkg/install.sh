#!/bin/bash -e

SCRIPT_PATH="$(readlink -f ${BASH_SOURCE[0]})"
SCRIPT_DIR=$(dirname $SCRIPT_PATH)
ZUS_DIR=${SCRIPT_DIR}/..

DEST_DIR=${1}
PKG_TYPE=${2:-rpm}

ZUS_LIB=${ZUS_DIR}/libzus.so
ZUS_BIN=${ZUS_DIR}/zusd
ZUS_SERVICE_NAME=zusd.service
ZUS_SERVICE=${SCRIPT_DIR}/${ZUS_SERVICE_NAME}
ZUS_ZUSD_HELPER_SCRIPT=${SCRIPT_DIR}/zusd.helper
FOOFS_LIB=${ZUS_DIR}/fs/foofs/libfoofs.so

SYSTEMD_SERVICE=/lib/systemd/system/${ZUS_SERVICE_NAME}
SYSTEMD_SERVICE_DEST=${DEST_DIR}${SYSTEMD_SERVICE}
SYSTEMD_DEPS_DIR=${DEST_DIR}/etc/systemd/system/multi-user.target.wants
ZUFS_LIB_DIR=${DEST_DIR}/usr/lib/zufs
ZUFS_LOG_DIR=${DEST_DIR}/var/log/zufs
SBIN_DIR=${DEST_DIR}/sbin

if [[ "${PKG_TYPE}" == "rpm" ]] ; then
	LIB64_DIR=${DEST_DIR}/usr/lib64
else
	LIB64_DIR=${DEST_DIR}/usr/lib/x86_64-linux-gnu
fi

mkdir -p $(dirname ${SYSTEMD_SERVICE_DEST}) ${SYSTEMD_DEPS_DIR} ${ZUFS_LIB_DIR} \
	 ${ZUFS_LOG_DIR} ${SBIN_DIR} ${LIB64_DIR}
cp -f ${ZUS_BIN} ${SBIN_DIR}
cp -f ${ZUS_LIB} ${LIB64_DIR}
if [[ -e ${FOOFS_LIB} ]]; then
	cp -f ${FOOFS_LIB} ${ZUFS_LIB_DIR}
fi
cp -f ${ZUS_ZUSD_HELPER_SCRIPT} ${ZUFS_LIB_DIR}
cp -f ${ZUS_SERVICE} ${SYSTEMD_SERVICE_DEST}
ln -sf ${SYSTEMD_SERVICE} ${SYSTEMD_DEPS_DIR}

[[ -z ${DEST_DIR} ]] && ${SCRIPT_DIR}/post_install.sh || true
