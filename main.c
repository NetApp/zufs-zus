/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * main.c - A CUI for the ZUS daemon
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 */

#define _GNU_SOURCE

#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <malloc.h>

#include "zus.h"
#include "zusd.h"

#ifdef CONFIG_ZUF_DEF_PATH

#define ZUF_DEF_PATH CONFIG_ZUF_DEF_PATH
#else
#define ZUF_DEF_PATH "/sys/fs/zuf"
#endif


static void usage(int argc, char *argv[])
{
	static char msg[] = {
	"usage: zus [options] [FILE_PATH]\n"
	"	--policyRR=[PRIORITY]\n"
	"		Set threads policy to SCHED_RR.\n"
	"		Optional PRIORITY is between 1-99. Default is 20\n"
	"		Only one of --policyRR --policyFIFO or --nice should be\n"
	"		specified, last one catches\n"
	"	--policyFIFO=[PRIORITY]\n"
	"		Set threads policy to SCHED_FIFO.(The default)\n"
	"		Optional PRIORITY is between 1-99. Default is 20\n"
	"		Only one of --policyRR --policyFIFO or --nice should be\n"
	"		specified, last one catches\n"
	"		--policyFIFO=20 is the default\n"
	"	--nice=[NICE_VAL]\n"
	"		Set threads policy to SCHED_OTHER.\n"
	"		And sets the nice value to NICE_VAL. Default NICE_VAL is 0\n"
	"		Only one of --policyRR --policyFIFO or --nice should be\n"
	"		specified, last one catches\n"
	"\n"
	"	FILE_PATH is the path to a mounted zuf-root directory\n"
	"\n"
	};
	char spf[2048];
	char *m = spf;
	uint s = sizeof(spf);
	int i, l;

	fprintf(stderr, msg);
	l = snprintf(m, s, "got: %s ", argv[0]);
	m += l; s -= l;
	for (i = 1; i < argc; ++i) {
		l = snprintf(m, s, "%s ", argv[i]);
		m += l; s -= l;
	}
	fprintf(stderr, "%s\n", spf);
}

int main(int argc, char *argv[])
{
	struct option opt[] = {
		{.name = "policyRR", .has_arg = 2, .flag = NULL, .val = 'r'},
		{.name = "policyFIFO", .has_arg = 2, .flag = NULL, .val = 'f'},
		{.name = "nice", .has_arg = 2, .flag = NULL, .val = 'n'},
		{.name = "verbose", .has_arg = 2, .flag = NULL, .val = 'd'},
		{.name = "mcheck", .has_arg = 0, .flag = NULL, .val = 'm'},
		{.name = 0, .has_arg = 0, .flag = 0, .val = 0},
	};
	const char *shortopt = "r::f::n::d::m";
	char op;
	struct zus_thread_params tp;
	const char *path = ZUF_DEF_PATH;
	int err;

	ZTP_INIT(&tp);
	while ((op = getopt_long(argc, argv, shortopt, opt, NULL)) != -1) {
		switch (op) {
		case 'r':
			tp.policy = SCHED_RR;
			if (optarg)
				tp.rr_priority = atoi(optarg);
			break;
		case 'f':
			tp.policy = SCHED_FIFO;
			if (optarg)
				tp.rr_priority = atoi(optarg);
			break;
		case 'n':
			tp.policy = SCHED_OTHER;
			if (optarg)
				tp.rr_priority = atoi(optarg);
			break;
		case 'd':
			if (optarg)
				g_DBGMASK = strtol(optarg, NULL, 0);
			else
				g_DBGMASK = 0x1;
			break;
		case 'm':
			mallopt(M_CHECK_ACTION, 3);
			break;
		default:
			/* Just ignore we are not the police */
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if ((argc < 0) || (argc > 1)) {
		usage(argc + optind, argv - optind);
		return 1;
	} else if (argc == 1) {
		path = argv[0];
	}

	zus_register_sigactions();

	err = zus_increase_max_files();
	if (unlikely(err))
		return err;

	err = zus_mount_thread_start(&tp, path);
	if (unlikely(err))
		goto stop;

	INFO("waiting for sigint ...\n");
	zus_join();

stop:
	zus_mount_thread_stop();
	return err;
}
