#!/bin/bash

SERVICE_NAME=zusd

echo "ZUFS_LIBFS_LIST=foofs" > ${DESTDIR}/etc/zufs.conf

# add libzus to ld DB
ldconfig

# configure and start daemon
systemctl -q is-active ${SERVICE_NAME} && systemctl stop ${SERVICE_NAME}
systemctl daemon-reload
systemctl start ${SERVICE_NAME}
