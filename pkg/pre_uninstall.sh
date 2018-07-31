#!/bin/bash

if [[ ${1} -ne 0 ]] ; then
	return
fi

SERVICE_NAME=zusd
SYSTEMD_DEPS_DIR=/etc/systemd/system/multi-user.target.wants

systemctl stop ${SERVICE_NAME}
rm -f ${SYSTEMD_DEPS_DIR}/${SERVICE_NAME}.service
systemctl daemon-reload

rm ${DESTDIR}/etc/zufs.conf
