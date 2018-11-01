#!/bin/bash

SERVICE_NAME=zusd
SYSTEMD_DEPS_DIR=/etc/systemd/system/multi-user.target.wants

# package removal, not upgrade
if [[ ${1} -eq 0 ]] ; then
	systemctl stop ${SERVICE_NAME}
	rm -f ${SYSTEMD_DEPS_DIR}/${SERVICE_NAME}.service
	systemctl daemon-reload
	rm -f /etc/zufs.conf
fi
