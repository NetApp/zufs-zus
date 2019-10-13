#!/bin/bash

CONF=/etc/zufs.conf

if [[ ! -f ${CONF} ]] ; then
	echo "ZUFS_LIBFS_LIST=" > ${CONF}
fi

# add libzus to ld DB
ldconfig

systemctl daemon-reload
systemctl restart zusd &>/dev/null || :
