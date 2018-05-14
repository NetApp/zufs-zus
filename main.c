/*
 * main.c - A CUI for the ZUS daemon
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boaz@plexistor.com>
 */

#define _GNU_SOURCE

#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>

#include "zus.h"
#include "zusd.h"

#ifdef CONFIG_ZUF_DEF_PATH

#define ZUF_DEF_PATH CONFIG_ZUF_DEF_PATH
#else
#define ZUF_DEF_PATH "/sys/fs/zuf"
#endif

bool g_DBG = false;
bool g_verify = false;
bool g_daemon = false;

static void usage(void)
{
	static char msg[] = {
	"usage: zus [options] [FILE_PATH]\n"
	"--policyRR=[PRIORITY]\n"
	"	Set threads policy to SCHED_RR.\n"
	"	Optional PRIORITY is between 1-99. Default is 20\n"
	"	Only one of --policyRR --policyFIFO or --nice should be\n"
	"	specified, last one catches\n"
	"--policyFIFO=[PRIORITY]\n"
	"	Set threads policy to SCHED_FIFO.(The default)\n"
	"	Optional PRIORITY is between 1-99. Default is 20\n"
	"	Only one of --policyRR --policyFIFO or --nice should be\n"
	"	specified, last one catches\n"
	"	--policyFIFO=20 is the default\n"
	"--nice=[NICE_VAL]\n"
	"	Set threads policy to SCHED_OTHER.\n"
	"	And sets the nice value to NICE_VAL. Default NICE_VAL is 0\n"
	"	Only one of --policyRR --policyFIFO or --nice should be\n"
	"	specified, last one catches\n"
	"--daemon\n"
	"	Run as daemon process\n"
	"\n"
	"FILE_PATH is the path to a mounted ZUS directory\n"
	"\n"
	};

	printf(msg);
}

static void sig_handler(int signo)
{
	printf("received sig(%d)\n", signo);
	zus_mount_thread_stop();
	exit(signo);
}

int main(int argc, char *argv[])
{
	struct option opt[] = {
		{.name = "policyRR", .has_arg = 2, .flag = NULL, .val = 'r'} ,
		{.name = "policyFIFO", .has_arg = 2, .flag = NULL, .val = 'f'} ,
		{.name = "nice", .has_arg = 2, .flag = NULL, .val = 'n'} ,
		{.name = "verbose", .has_arg = 0, .flag = NULL, .val = 'd'} ,
		{.name = "verify", .has_arg = 0, .flag = NULL, .val = 'v'} ,
		{.name = "daemon", .has_arg = 0, .flag = NULL, .val = 'D'} ,
		{.name = 0, .has_arg = 0, .flag = 0, .val = 0} ,
	};
	char op;
	struct thread_param tp = {
		.path = ZUF_DEF_PATH,
		.policy = SCHED_FIFO,
		.rr_priority = 20,
	};
	int err;

	while ((op = getopt_long(argc, argv, "w::rm::dD", opt, NULL)) != -1) {
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
			g_DBG = true;
			break;
		case 'v':
			g_verify = true;
			break;
		case 'D':
			g_daemon = true;
			break;
		default:;
			/* Just ignore we are not the police */
		}
	}

	argc -= optind;
	argv += optind;

	if ((argc < 0) || (argc > 1)) {
		usage();
		return 1;
	} else if (argc == 1) {
		tp.path = argv[0];
	}

	if (signal(SIGINT, sig_handler) == SIG_ERR)
		ERROR("signal SIGINT not installed\n");

	if (g_daemon && daemon(0, 1))
		ERROR("daemon failed\n");

	err = zus_mount_thread_start(&tp);
	if (unlikely(err))
		goto stop;

	INFO("waiting for sigint ...\n");
	zus_join();

stop:
	zus_mount_thread_stop();
	return err;
}
