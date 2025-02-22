/*
 * Copyright (C) 2012 Alexander Block.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */


#include "kerncompat.h"

#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <libgen.h>
#include <mntent.h>
#include <assert.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include <limits.h>

#include "ctree.h"
#include "ioctl.h"
#include "commands.h"
#include "list.h"
#include "utils.h"

#include "send.h"
#include "send-utils.h"

static int g_verbose = 0;

struct btrfs_send {
	int send_fd;
	int dump_fd;
	int mnt_fd;

	u64 *clone_sources;
	u64 clone_sources_count;

	char *root_path;
	struct subvol_uuid_search sus;
};

static int get_root_id(struct btrfs_send *s, const char *path, u64 *root_id)
{
	struct subvol_info *si;

	si = subvol_uuid_search(&s->sus, 0, NULL, 0, path,
			subvol_search_by_path);
	if (!si)
		return -ENOENT;
	*root_id = si->root_id;
	free(si->path);
	free(si);
	return 0;
}

static struct subvol_info *get_parent(struct btrfs_send *s, u64 root_id)
{
	struct subvol_info *si_tmp;
	struct subvol_info *si;

	si_tmp = subvol_uuid_search(&s->sus, root_id, NULL, 0, NULL,
			subvol_search_by_root_id);
	if (!si_tmp)
		return NULL;

	si = subvol_uuid_search(&s->sus, 0, si_tmp->parent_uuid, 0, NULL,
			subvol_search_by_uuid);
	free(si_tmp->path);
	free(si_tmp);
	return si;
}

static int find_good_parent(struct btrfs_send *s, u64 root_id, u64 *found)
{
	int ret;
	struct subvol_info *parent = NULL;
	struct subvol_info *parent2 = NULL;
	struct subvol_info *best_parent = NULL;
	__s64 tmp;
	u64 best_diff = (u64)-1;
	int i;

	parent = get_parent(s, root_id);
	if (!parent) {
		ret = -ENOENT;
		goto out;
	}

	for (i = 0; i < s->clone_sources_count; i++) {
		if (s->clone_sources[i] == parent->root_id) {
			best_parent = parent;
			parent = NULL;
			goto out_found;
		}
	}

	for (i = 0; i < s->clone_sources_count; i++) {
		parent2 = get_parent(s, s->clone_sources[i]);
		if (!parent2)
			continue;
		if (parent2->root_id != parent->root_id) {
			free(parent2->path);
			free(parent2);
			parent2 = NULL;
			continue;
		}

		free(parent2->path);
		free(parent2);
		parent2 = subvol_uuid_search(&s->sus, s->clone_sources[i], NULL,
				0, NULL, subvol_search_by_root_id);

		if (!parent2) {
			ret = -ENOENT;
			goto out;
		}
		tmp = parent2->ctransid - parent->ctransid;
		if (tmp < 0)
			tmp *= -1;
		if (tmp < best_diff) {
			if (best_parent) {
				free(best_parent->path);
				free(best_parent);
			}
			best_parent = parent2;
			parent2 = NULL;
			best_diff = tmp;
		} else {
			free(parent2->path);
			free(parent2);
			parent2 = NULL;
		}
	}

	if (!best_parent) {
		ret = -ENOENT;
		goto out;
	}

out_found:
	*found = best_parent->root_id;
	ret = 0;

out:
	if (parent) {
		free(parent->path);
		free(parent);
	}
	if (best_parent) {
		free(best_parent->path);
		free(best_parent);
	}
	return ret;
}

static int add_clone_source(struct btrfs_send *s, u64 root_id)
{
	void *tmp;

	tmp = s->clone_sources;
	s->clone_sources = realloc(s->clone_sources,
		sizeof(*s->clone_sources) * (s->clone_sources_count + 1));

	if (!s->clone_sources) {
		free(tmp);
		return -ENOMEM;
	}
	s->clone_sources[s->clone_sources_count++] = root_id;

	return 0;
}

static int write_buf(int fd, const void *buf, int size)
{
	int ret;
	int pos = 0;

	while (pos < size) {
		ret = write(fd, (char*)buf + pos, size - pos);
		if (ret < 0) {
			ret = -errno;
			error("failed to dump stream: %s", strerror(-ret));
			goto out;
		}
		if (!ret) {
			ret = -EIO;
			error("failed to dump stream: %s", strerror(-ret));
			goto out;
		}
		pos += ret;
	}
	ret = 0;

out:
	return ret;
}

static void *dump_thread(void *arg_)
{
	int ret;
	struct btrfs_send *s = (struct btrfs_send*)arg_;
	char buf[4096];
	int readed;

	while (1) {
		readed = read(s->send_fd, buf, sizeof(buf));
		if (readed < 0) {
			ret = -errno;
			error("failed to read stream from kernel: %s\n",
				strerror(-ret));
			goto out;
		}
		if (!readed) {
			ret = 0;
			goto out;
		}
		ret = write_buf(s->dump_fd, buf, readed);
		if (ret < 0)
			goto out;
	}

out:
	if (ret < 0) {
		exit(-ret);
	}

	return ERR_PTR(ret);
}

static int do_send(struct btrfs_send *send, u64 parent_root_id,
		   int is_first_subvol, int is_last_subvol, char *subvol,
		   u64 flags)
{
	int ret;
	pthread_t t_read;
	struct btrfs_ioctl_send_args io_send;
	void *t_err = NULL;
	int subvol_fd = -1;
	int pipefd[2] = {-1, -1};

	subvol_fd = openat(send->mnt_fd, subvol, O_RDONLY | O_NOATIME);
	if (subvol_fd < 0) {
		ret = -errno;
		error("cannot open %s: %s", subvol, strerror(-ret));
		goto out;
	}

	ret = pipe(pipefd);
	if (ret < 0) {
		ret = -errno;
		error("pipe failed: %s", strerror(-ret));
		goto out;
	}

	memset(&io_send, 0, sizeof(io_send));
	io_send.send_fd = pipefd[1];
	send->send_fd = pipefd[0];

	if (!ret)
		ret = pthread_create(&t_read, NULL, dump_thread,
					send);
	if (ret) {
		ret = -ret;
		error("thread setup failed: %s", strerror(-ret));
		goto out;
	}

	io_send.flags = flags;
	io_send.clone_sources = (__u64*)send->clone_sources;
	io_send.clone_sources_count = send->clone_sources_count;
	io_send.parent_root = parent_root_id;
	if (!is_first_subvol)
		io_send.flags |= BTRFS_SEND_FLAG_OMIT_STREAM_HEADER;
	if (!is_last_subvol)
		io_send.flags |= BTRFS_SEND_FLAG_OMIT_END_CMD;
	ret = ioctl(subvol_fd, BTRFS_IOC_SEND, &io_send);
	if (ret < 0) {
		ret = -errno;
		error("send ioctl failed with %d: %s", ret, strerror(-ret));
		if (ret == -EINVAL && (!is_first_subvol || !is_last_subvol))
			fprintf(stderr,
				"Try upgrading your kernel or don't use -e.\n");
		goto out;
	}
	if (g_verbose > 0)
		fprintf(stderr, "BTRFS_IOC_SEND returned %d\n", ret);

	if (g_verbose > 0)
		fprintf(stderr, "joining genl thread\n");

	close(pipefd[1]);
	pipefd[1] = -1;

	ret = pthread_join(t_read, &t_err);
	if (ret) {
		ret = -ret;
		error("pthread_join failed: %s", strerror(-ret));
		goto out;
	}
	if (t_err) {
		ret = (long int)t_err;
		error("failed to process send stream, ret=%ld (%s)",
				(long int)t_err, strerror(-ret));
		goto out;
	}

	ret = 0;

out:
	if (subvol_fd != -1)
		close(subvol_fd);
	if (pipefd[0] != -1)
		close(pipefd[0]);
	if (pipefd[1] != -1)
		close(pipefd[1]);
	return ret;
}

static int init_root_path(struct btrfs_send *s, const char *subvol)
{
	int ret = 0;

	if (s->root_path)
		goto out;

	ret = find_mount_root(subvol, &s->root_path);
	if (ret < 0) {
		error("failed to determine mount point for %s: %s",
			subvol, strerror(-ret));
		ret = -EINVAL;
		goto out;
	}
	if (ret > 0) {
		error("%s doesn't belong to btrfs mount point", subvol);
		ret = -EINVAL;
		goto out;
	}

	s->mnt_fd = open(s->root_path, O_RDONLY | O_NOATIME);
	if (s->mnt_fd < 0) {
		ret = -errno;
		error("cannot open '%s': %s", s->root_path, strerror(-ret));
		goto out;
	}

	ret = subvol_uuid_search_init(s->mnt_fd, &s->sus);
	if (ret < 0) {
		error("failed to initialize subvol search: %s",
			strerror(-ret));
		goto out;
	}

out:
	return ret;

}

static int is_subvol_ro(struct btrfs_send *s, char *subvol)
{
	int ret;
	u64 flags;
	int fd = -1;

	fd = openat(s->mnt_fd, subvol, O_RDONLY | O_NOATIME);
	if (fd < 0) {
		ret = -errno;
		error("cannot open %s: %s", subvol, strerror(-ret));
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_SUBVOL_GETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		error("failed to get flags for subvolume %s: %s",
			subvol, strerror(-ret));
		goto out;
	}

	if (flags & BTRFS_SUBVOL_RDONLY)
		ret = 1;
	else
		ret = 0;

out:
	if (fd != -1)
		close(fd);

	return ret;
}

int cmd_send(int argc, char **argv)
{
	char *subvol = NULL;
	int ret;
	char outname[PATH_MAX];
	struct btrfs_send send;
	u32 i;
	char *mount_root = NULL;
	char *snapshot_parent = NULL;
	u64 root_id = 0;
	u64 parent_root_id = 0;
	int full_send = 1;
	int new_end_cmd_semantic = 0;
	u64 send_flags = 0;

	memset(&send, 0, sizeof(send));
	send.dump_fd = fileno(stdout);
	outname[0] = 0;

	while (1) {
		enum { GETOPT_VAL_SEND_NO_DATA = 256 };
		static const struct option long_options[] = {
			{ "no-data", no_argument, NULL, GETOPT_VAL_SEND_NO_DATA }
		};
		int c = getopt_long(argc, argv, "vec:f:i:p:", long_options, NULL);

		if (c < 0)
			break;

		switch (c) {
		case 'v':
			g_verbose++;
			break;
		case 'e':
			new_end_cmd_semantic = 1;
			break;
		case 'c':
			subvol = realpath(optarg, NULL);
			if (!subvol) {
				ret = -errno;
				error("realpath %s failed: %s\n", optarg, strerror(-ret));
				goto out;
			}

			ret = init_root_path(&send, subvol);
			if (ret < 0)
				goto out;

			ret = get_root_id(&send,
				subvol_strip_mountpoint(send.root_path, subvol),
				&root_id);
			if (ret < 0) {
				error("cannot resolve rootid for %s", subvol);
				goto out;
			}

			ret = is_subvol_ro(&send, subvol);
			if (ret < 0)
				goto out;
			if (!ret) {
				ret = -EINVAL;
				error("cloned subvolume %s is not read-only", subvol);
				goto out;
			}

			ret = add_clone_source(&send, root_id);
			if (ret < 0) {
				error("cannot add clone source: %s", strerror(-ret));
				goto out;
			}
			subvol_uuid_search_finit(&send.sus);
			free(subvol);
			subvol = NULL;
			if (send.mnt_fd >= 0) {
				close(send.mnt_fd);
				send.mnt_fd = -1;
			}
			free(send.root_path);
			send.root_path = NULL;
			full_send = 0;
			break;
		case 'f':
			if (arg_copy_path(outname, optarg, sizeof(outname))) {
				error("output file path too long (%zu)", strlen(optarg));
				ret = 1;
				goto out;
			}
			break;
		case 'p':
			if (snapshot_parent) {
				error("you cannot have more than one parent (-p)");
				ret = 1;
				goto out;
			}
			snapshot_parent = realpath(optarg, NULL);
			if (!snapshot_parent) {
				ret = -errno;
				error("realpath %s failed: %s", optarg, strerror(-ret));
				goto out;
			}

			ret = is_subvol_ro(&send, snapshot_parent);
			if (ret < 0)
				goto out;
			if (!ret) {
				ret = -EINVAL;
				error("parent subvolume %s is not read-only",
					snapshot_parent);
				goto out;
			}

			full_send = 0;
			break;
		case 'i':
			error("option -i was removed, use -c instead");
			ret = 1;
			goto out;
		case GETOPT_VAL_SEND_NO_DATA:
			send_flags |= BTRFS_SEND_FLAG_NO_FILE_DATA;
			break;
		case '?':
		default:
			error("send arguments invalid");
			ret = 1;
			goto out;
		}
	}

	if (check_argc_min(argc - optind, 1))
		usage(cmd_send_usage);

	if (outname[0]) {
		send.dump_fd = creat(outname, 0600);
		if (send.dump_fd == -1) {
			ret = -errno;
			error("cannot create '%s': %s", outname, strerror(-ret));
			goto out;
		}
	}

	if (isatty(send.dump_fd)) {
		error(
	    "not dumping send stream into a terminal, redirect it into a file");
		ret = 1;
		goto out;
	}

	/* use first send subvol to determine mount_root */
	subvol = argv[optind];

	subvol = realpath(argv[optind], NULL);
	if (!subvol) {
		ret = -errno;
		error("unable to resolve %s", argv[optind]);
		goto out;
	}

	ret = init_root_path(&send, subvol);
	if (ret < 0)
		goto out;

	if (snapshot_parent != NULL) {
		ret = get_root_id(&send,
			subvol_strip_mountpoint(send.root_path, snapshot_parent),
			&parent_root_id);
		if (ret < 0) {
			error("could not resolve rootid for %s", snapshot_parent);
			goto out;
		}

		ret = add_clone_source(&send, parent_root_id);
		if (ret < 0) {
			error("cannot add clone source: %s", strerror(-ret));
			goto out;
		}
	}

	for (i = optind; i < argc; i++) {
		free(subvol);
		subvol = realpath(argv[i], NULL);
		if (!subvol) {
			ret = -errno;
			error("unable to resolve %s", argv[i]);
			goto out;
		}

		ret = find_mount_root(subvol, &mount_root);
		if (ret < 0) {
			error("find_mount_root failed on %s: %s", subvol,
				strerror(-ret));
			goto out;
		}
		if (ret > 0) {
			error("%s does not belong to btrfs mount point",
				subvol);
			ret = -EINVAL;
			goto out;
		}
		if (strcmp(send.root_path, mount_root) != 0) {
			ret = -EINVAL;
			error("all subvolumes must be from the same filesystem");
			goto out;
		}
		free(mount_root);

		ret = is_subvol_ro(&send, subvol);
		if (ret < 0)
			goto out;
		if (!ret) {
			ret = -EINVAL;
			error("subvolum %s is not read-only", subvol);
			goto out;
		}
	}

	if (send_flags & BTRFS_SEND_FLAG_NO_FILE_DATA)
		printf("Mode NO_FILE_DATA enabled\n");

	for (i = optind; i < argc; i++) {
		int is_first_subvol;
		int is_last_subvol;

		free(subvol);
		subvol = argv[i];

		fprintf(stderr, "At subvol %s\n", subvol);

		subvol = realpath(subvol, NULL);
		if (!subvol) {
			ret = -errno;
			error("realpath %s failed: %s", argv[i], strerror(-ret));
			goto out;
		}

		if (!full_send && !parent_root_id) {
			ret = find_good_parent(&send, root_id, &parent_root_id);
			if (ret < 0) {
				error("parent determination failed for %lld",
					root_id);
				goto out;
			}
		}

		ret = is_subvol_ro(&send, subvol);
		if (ret < 0)
			goto out;
		if (!ret) {
			ret = -EINVAL;
			error("subvolume %s is not read-only", subvol);
			goto out;
		}

		if (new_end_cmd_semantic) {
			/* require new kernel */
			is_first_subvol = (i == optind);
			is_last_subvol = (i == argc - 1);
		} else {
			/* be compatible to old and new kernel */
			is_first_subvol = 1;
			is_last_subvol = 1;
		}
		ret = do_send(&send, parent_root_id, is_first_subvol,
			      is_last_subvol, subvol, send_flags);
		if (ret < 0)
			goto out;

		if (!full_send) {
			/* done with this subvol, so add it to the clone sources */
			ret = add_clone_source(&send, root_id);
			if (ret < 0) {
				error("cannot add clone source: %s", strerror(-ret));
				goto out;
			}
		}

		parent_root_id = 0;
	}

	ret = 0;

out:
	free(subvol);
	free(snapshot_parent);
	free(send.clone_sources);
	if (send.mnt_fd >= 0)
		close(send.mnt_fd);
	free(send.root_path);
	subvol_uuid_search_finit(&send.sus);
	return !!ret;
}

const char * const cmd_send_usage[] = {
	"btrfs send [-ve] [-p <parent>] [-c <clone-src>] [-f <outfile>] <subvol> [<subvol>...]",
	"Send the subvolume(s) to stdout.",
	"Sends the subvolume(s) specified by <subvol> to stdout.",
	"<subvol> should be read-only here.",
	"By default, this will send the whole subvolume. To do an incremental",
	"send, use '-p <parent>'. If you want to allow btrfs to clone from",
	"any additional local snapshots, use '-c <clone-src>' (multiple times",
	"where applicable). You must not specify clone sources unless you",
	"guarantee that these snapshots are exactly in the same state on both",
	"sides, the sender and the receiver. It is allowed to omit the",
	"'-p <parent>' option when '-c <clone-src>' options are given, in",
	"which case 'btrfs send' will determine a suitable parent among the",
	"clone sources itself.",
	"\n",
	"-v               Enable verbose debug output. Each occurrence of",
	"                 this option increases the verbose level more.",
	"-e               If sending multiple subvols at once, use the new",
	"                 format and omit the end-cmd between the subvols.",
	"-p <parent>      Send an incremental stream from <parent> to",
	"                 <subvol>.",
	"-c <clone-src>   Use this snapshot as a clone source for an ",
	"                 incremental send (multiple allowed)",
	"-f <outfile>     Output is normally written to stdout. To write to",
	"                 a file, use this option. An alternative would be to",
	"                 use pipes.",
	"--no-data        send in NO_FILE_DATA mode, Note: the output stream",
	"                 does not contain any file data and thus cannot be used",
	"                 to transfer changes. This mode is faster and useful to",
	"                 show the differences in metadata.",
	NULL
};
