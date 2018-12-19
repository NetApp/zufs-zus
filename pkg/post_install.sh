#!/bin/bash

SERVICE_NAME=zusd
CONF=/etc/zufs.conf

if [[ ! -f ${CONF} ]] ; then
	echo "ZUFS_LIBFS_LIST=" > ${CONF}
fi

# add libzus to ld DB
ldconfig

# configure and start daemon
systemctl -q is-active ${SERVICE_NAME} && systemctl stop ${SERVICE_NAME}
systemctl daemon-reload
