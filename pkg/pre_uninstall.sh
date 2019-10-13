#!/bin/bash

# package removal, not upgrade
if [[ ${1} -eq 0 ]] ; then
	systemctl stop zusd &>/dev/null || :
	systemctl daemon-reload
	rm -f /etc/zufs.conf
	ldconfig
fi
