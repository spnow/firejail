/*
 * Copyright (C) 2014 netblue30 (netblue30@yahoo.com)
 *
 * This file is part of firejail project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <string.h>
#include "firejail.h"

static int apply_caps = 0;
static uint64_t caps = 0;
static int apply_seccomp = 0;


#define BUFLEN 4096
static extract_caps_seccomp(pid_t pid) {
	// open stat file
	char *file;
	if (asprintf(&file, "/proc/%u/status", pid) == -1) {
		perror("asprintf");
		exit(1);
	}
	FILE *fp = fopen(file, "r");
	if (!fp) {
		free(file);
		fprintf(stderr, "Error: cannot find process %u\n", pid);
		exit(1);
	}

	char buf[BUFLEN];
	while (fgets(buf, BUFLEN - 1, fp)) {
		if (strncmp(buf, "Seccomp:", 8) == 0) {
			char *ptr = buf + 8;
			int val;
			sscanf(ptr, "%d", &val);
			if (val == 2)
				apply_seccomp = 1;
			break;
		}
		else if (strncmp(buf, "CapBnd:", 7) == 0) {		
			char *ptr = buf + 8;
			unsigned long long val;
			sscanf(ptr, "%llx", &val);
			apply_caps = 1;
			caps = val;
		}
	}
	fclose(fp);
	free(file);
}

void join_name(const char *name, const char *homedir, const char *command) {
	if (!name || strlen(name) == 0) {
		fprintf(stderr, "Error: invalid sandbox name\n");
		exit(1);
	}
	pid_t pid;
	if (name2pid(name, &pid)) {
		fprintf(stderr, "Error: cannot find sandbox %s\n", name);
		exit(1);
	}

	join(pid, homedir, command);
}

void join(pid_t pid, const char *homedir, const char *command) {
	// if the pid is that of a firejail  process, use the pid of a child process inside the sandbox
	char *comm = pid_proc_comm(pid);
	if (comm) {
		// remove \n
		char *ptr = strchr(comm, '\n');
		if (ptr)
			*ptr = '\0';
		if (strcmp(comm, "firejail") == 0) {
			pid_t child;
			if (find_child(pid, &child) == 0) {
				pid = child;
				printf("Switching to pid %u, the first child process inside the sandbox\n", (unsigned) pid);
			}
		}
		free(comm);
	}

	// in user mode apply caps and seccomp filters
	if (getuid() != 0)
		extract_caps_seccomp(pid);

	// check privileges for non-root users
	uid_t uid = getuid();
	if (uid != 0) {
		struct stat s;
		char *dir;
		if (asprintf(&dir, "/proc/%u/ns", pid) == -1)
			errExit("asprintf");
		if (stat(dir, &s) < 0)
			errExit("stat");
		if (s.st_uid != uid) {
			fprintf(stderr, "Error: permission is denied to join a sandbox created by a different user.\n");
			exit(1);
		}
	}

	// join namespaces
	if (join_namespace(pid, "ipc"))
		exit(1);
	if (join_namespace(pid, "net"))
		exit(1);
	if (join_namespace(pid, "pid"))
		exit(1);
	if (join_namespace(pid, "uts"))
		exit(1);
	if (join_namespace(pid, "mnt"))
		exit(1);

	pid_t child = fork();
	if (child < 0)
		errExit("fork");
	if (child == 0) {
		// chroot into /proc/PID/root directory
		char *rootdir;
		if (asprintf(&rootdir, "/proc/%d/root", pid) == -1)
			errExit("asprintf");
			
		int rv = chroot(rootdir); // this will fail for processes in sandboxes not started with --chroot option
		if (rv == 0)
			printf("changing root to %s\n", rootdir);
		
		prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0); // kill the child in case the parent died
		if (chdir("/") < 0)
			errExit("chdir");
		if (homedir) {
			struct stat s;
			if (stat(homedir, &s) == 0) {
				if (chdir(homedir) < 0)
					errExit("chdir");
			}
		}
		
		// set caps filter
		if (apply_caps == 1)
			caps_set(caps);
#ifdef HAVE_SECCOMP
		// set seccomp filter
		if (apply_seccomp == 1)
			seccomp_set();
#endif
		
		// fix qt 4.8
		if (setenv("QT_X11_NO_MITSHM", "1", 1) < 0)
			errExit("setenv");
		if (setenv("container", "firejail", 1) < 0) // LXC sets container=lxc,
			errExit("setenv");
		// drop privileges
		drop_privs();

		// set prompt color to green
		//export PS1='\[\e[1;32m\][\u@\h \W]\$\[\e[0m\] '
		if (setenv("PROMPT_COMMAND", "export PS1=\"\\[\\e[1;32m\\][\\u@\\h \\W]\\$\\[\\e[0m\\] \"", 1) < 0)
			errExit("setenv");

		// set the shell
		char *sh;
		if (cfg.shell)
 			sh = cfg.shell;
		else if (arg_zsh)
			sh = "/usr/bin/zsh";
		else if (arg_csh)
			sh = "/bin/csh";
		else
			sh = "/bin/bash";
		
		char *arg[4];
		arg[0] = sh;
		arg[1] = "-c";
		assert(cfg.command_line);
		if (arg_debug)
			printf("Starting %s\n", cfg.command_line);
		arg[2] = cfg.command_line;
		arg[3] = NULL;

		// launch command
		execvp(sh, arg);
		// it will never get here!!!
	}

	// wait for the child to finish
	waitpid(child, NULL, 0);
	exit(0);
}



