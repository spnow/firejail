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
#include "firemon.h"

#define PIDS_BUFLEN 4096
static int pid_is_firejail(pid_t pid) {
	uid_t rv = 0;
	
	// open stat file
	char *file;
	if (asprintf(&file, "/proc/%u/status", pid) == -1) {
		perror("asprintf");
		exit(1);
	}
	FILE *fp = fopen(file, "r");
	if (!fp) {
		free(file);
		return 0;
	}

	// look for firejail executable name
	char buf[PIDS_BUFLEN];
	while (fgets(buf, PIDS_BUFLEN - 1, fp)) {
		if (strncmp(buf, "Name:", 5) == 0) {
			char *ptr = buf + 5;
			while (*ptr != '\0' && (*ptr == ' ' || *ptr == '\t')) {
				ptr++;
			}
			if (*ptr == '\0')
				goto doexit;
			if (strncmp(ptr, "firejail", 8) == 0)
				rv = 1;
//			if (strncmp(ptr, "lxc-execute", 11) == 0)
//				rv = 1;
			break;
		}
	}
doexit:	
	fclose(fp);
	free(file);
	return rv;
}


static int procevent_netlink_setup(void) {
	// open socket for process event connector
	int sock;
	if ((sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR)) < 0) {
		fprintf(stderr, "Error: cannot open netlink socket\n");
		exit(1);
	}

	// bind socket
	struct sockaddr_nl addr;
	memset(&addr, 0, sizeof(addr));
	addr.nl_pid = getpid();
	addr.nl_family = AF_NETLINK;
	addr.nl_groups = CN_IDX_PROC;
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "Error: cannot bind to netlink socket\n");
		exit(1);
	}

	// send monitoring message
	struct nlmsghdr nlmsghdr;
	memset(&nlmsghdr, 0, sizeof(nlmsghdr));
	nlmsghdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct cn_msg) + sizeof(enum proc_cn_mcast_op));
	nlmsghdr.nlmsg_pid = getpid();
	nlmsghdr.nlmsg_type = NLMSG_DONE;

	struct cn_msg cn_msg;
	memset(&cn_msg, 0, sizeof(cn_msg));
	cn_msg.id.idx = CN_IDX_PROC;
	cn_msg.id.val = CN_VAL_PROC;
	cn_msg.len = sizeof(enum proc_cn_mcast_op);

	struct iovec iov[3];
	iov[0].iov_base = &nlmsghdr;
	iov[0].iov_len = sizeof(nlmsghdr);
	iov[1].iov_base = &cn_msg;
	iov[1].iov_len = sizeof(cn_msg);

	enum proc_cn_mcast_op op = PROC_CN_MCAST_LISTEN;
	iov[2].iov_base = &op;
	iov[2].iov_len = sizeof(op);

	if (writev(sock, iov, 3) == -1) {
		fprintf(stderr, "Error: cannot write to netlink socket\n");
		exit(1);
	}
	
	return sock;
}

static int procevent_monitor(const int sock, pid_t mypid) {
	ssize_t len;
	struct nlmsghdr *nlmsghdr;

	while (1) {
		char __attribute__ ((aligned(NLMSG_ALIGNTO)))buf[4096];

		if ((len = recv(sock, buf, sizeof(buf), 0)) == 0) {
			return 0;
		}
		if (len == -1) {
			if (errno == EINTR) {
				return 0;
			} else {
				fprintf(stderr,"recv: %s\n", strerror(errno));
				return -1;
			}
		}

		for (nlmsghdr = (struct nlmsghdr *)buf;
			NLMSG_OK (nlmsghdr, len);
			nlmsghdr = NLMSG_NEXT (nlmsghdr, len)) {

			struct cn_msg *cn_msg;
			struct proc_event *proc_ev;
			struct tm tm;
			struct timeval tv;
			time_t now;

			if ((nlmsghdr->nlmsg_type == NLMSG_ERROR) ||
			    (nlmsghdr->nlmsg_type == NLMSG_NOOP))
				continue;

			cn_msg = NLMSG_DATA(nlmsghdr);
			if ((cn_msg->id.idx != CN_IDX_PROC) ||
			    (cn_msg->id.val != CN_VAL_PROC))
				continue;

			(void)time(&now);
			(void)localtime_r(&now, &tm);
			char line[PIDS_BUFLEN];
			char *lineptr = line;
			sprintf(lineptr, "%2.2d:%2.2d:%2.2d", tm.tm_hour, tm.tm_min, tm.tm_sec);
			lineptr += strlen(lineptr);

			proc_ev = (struct proc_event *)cn_msg->data;
			pid_t pid = 0;
			pid_t child = 0;
			int remove_pid = 0;
			switch (proc_ev->what) {
				case PROC_EVENT_FORK:
					if (proc_ev->event_data.fork.child_pid !=
					    proc_ev->event_data.fork.child_tgid)
					    	continue; // this is a thread, not a process
					pid = proc_ev->event_data.fork.parent_tgid;
					if (pids[pid].level > 0) {
						child = proc_ev->event_data.fork.child_tgid;
						child %= MAX_PIDS;
						pids[child].level = pids[pid].level + 1;
						pids[child].uid = pid_get_uid(child);
					}
					sprintf(lineptr, " fork");
					break;
				case PROC_EVENT_EXEC:
					pid = proc_ev->event_data.exec.process_tgid;
					sprintf(lineptr, " exec");
					break;
					
				case PROC_EVENT_EXIT:
					if (proc_ev->event_data.exit.process_pid !=
					    proc_ev->event_data.exit.process_tgid)
						continue; // this is a thread, not a process

					pid = proc_ev->event_data.exit.process_tgid;
					remove_pid = 1;
					sprintf(lineptr, " exit");
					break;
					
				case PROC_EVENT_UID:
					pid = proc_ev->event_data.id.process_tgid;
					sprintf(lineptr, " uid ");
					break;

				case PROC_EVENT_GID:
					pid = proc_ev->event_data.id.process_tgid;
					sprintf(lineptr, " gid ");
					break;

				case PROC_EVENT_SID:
					pid = proc_ev->event_data.sid.process_tgid;
					sprintf(lineptr, " sid ");
					break;

				default:
					sprintf(lineptr, "\n");
					continue;
			}

			int add_new = 0;
			if (pids[pid].level < 0)	// not a firejail process
				continue;
			else if (pids[pid].level == 0) { // new porcess, do we have track it?
				if (pid_is_firejail(pid) && mypid == 0) {
					pids[pid].level = 1;
					add_new = 1;
				}
				else {
					pids[pid].level = -1;
					continue;
				}
			}
				
			lineptr += strlen(lineptr);
			sprintf(lineptr, " %u", pid);
			lineptr += strlen(lineptr);
			
			char *user = pids[pid].user;
			if (!user)
				user = pid_get_user_name(pids[pid].uid);
			if (user) {
				pids[pid].user = user;
				sprintf(lineptr, " (%s)", user);
				lineptr += strlen(lineptr);
			}
			

			int sandbox_closed = 0; // exit sandbox flag
			char *cmd = pids[pid].cmd;
			if (add_new) {
				sprintf(lineptr, " NEW SANDBOX\n");
				lineptr += strlen(lineptr);
			}
			else if (proc_ev->what == PROC_EVENT_EXIT && pids[pid].level == 1) {
				sprintf(lineptr, " EXIT SANDBOX\n");
				lineptr += strlen(lineptr);
				if (mypid == pid)
					sandbox_closed = 1;
			}
			else {
				if (!cmd) {
					cmd = pid_proc_cmdline(pid);
				}
				if (cmd == NULL)
					sprintf(lineptr, "\n");
				else {
					sprintf(lineptr, " %s\n", cmd);
					free(cmd);
				}
				lineptr += strlen(lineptr);
			}
			(void) lineptr;
			
			// print the event
			printf("%s", line);			
			fflush(0);
			
			// unflag pid for exit events
			if (remove_pid) {
				if (pids[pid].user)
					free(pids[pid].user);
				if (pids[pid].cmd)
					free(pids[pid].cmd);
				memset(&pids[pid], 0, sizeof(Process));
			}

			// print forked child
			if (child) {
				cmd = pid_proc_cmdline(child);
				if (cmd) {
					printf("\tchild %u %s\n", child, cmd);
					free(cmd);
				}
				else
					printf("\tchild %u\n", child);
			}
			
			// on uid events the uid is changing
			if (proc_ev->what == PROC_EVENT_UID) {
				if (pids[pid].user)
					free(pids[pid].user);
				pids[pid].user = 0;
				pids[pid].uid = pid_get_uid(pid); 
			}
			
			if (sandbox_closed)
				exit(0);
		}
	}
	return 0;
}

static void procevent_print_pids(void) {
	// print files
	int i;
	for (i = 0; i < MAX_PIDS; i++) {
		if (pids[i].level == 1)
			pid_print_tree(i, 0, 1);
	}
	printf("\n");
}

void procevent(pid_t pid) {
	// need to be root for this
	if (getuid() != 0) {
		fprintf(stderr, "Error: you need to be root to get process events\n");
		exit(1);
	}

	// read and print sandboxed processes
	pid_read(pid);
	procevent_print_pids();

	// monitor using netlink
	int sock = procevent_netlink_setup();
	if (sock < 0) {
		fprintf(stderr, "Error: cannot open netlink socket\n");
		exit(1);
	}
	procevent_monitor(sock,pid); // it will never return from here
}
