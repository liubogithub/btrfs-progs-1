/*
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <uuid/uuid.h>
#include <ctype.h>
#include <fcntl.h>
#include <ftw.h>
#include <mntent.h>
#include <linux/limits.h>
#include <getopt.h>

#include "kerncompat.h"
#include "ctree.h"
#include "ioctl.h"
#include "utils.h"
#include "volumes.h"
#include "commands.h"
#include "cmds-fi-usage.h"
#include "list_sort.h"
#include "disk-io.h"
#include "cmds-fi-du.h"

/*
 * for btrfs fi show, we maintain a hash of fsids we've already printed.
 * This way we don't print dups if a given FS is mounted more than once.
 */
#define SEEN_FSID_HASH_SIZE 256

struct seen_fsid {
	u8 fsid[BTRFS_FSID_SIZE];
	struct seen_fsid *next;
};

static struct seen_fsid *seen_fsid_hash[SEEN_FSID_HASH_SIZE] = {NULL,};

static int is_seen_fsid(u8 *fsid)
{
	u8 hash = fsid[0];
	int slot = hash % SEEN_FSID_HASH_SIZE;
	struct seen_fsid *seen = seen_fsid_hash[slot];

	return seen ? 1 : 0;
}

static int add_seen_fsid(u8 *fsid)
{
	u8 hash = fsid[0];
	int slot = hash % SEEN_FSID_HASH_SIZE;
	struct seen_fsid *seen = seen_fsid_hash[slot];
	struct seen_fsid *alloc;

	if (!seen)
		goto insert;

	while (1) {
		if (memcmp(seen->fsid, fsid, BTRFS_FSID_SIZE) == 0)
			return -EEXIST;

		if (!seen->next)
			break;

		seen = seen->next;
	}

insert:

	alloc = malloc(sizeof(*alloc));
	if (!alloc)
		return -ENOMEM;

	alloc->next = NULL;
	memcpy(alloc->fsid, fsid, BTRFS_FSID_SIZE);

	if (seen)
		seen->next = alloc;
	else
		seen_fsid_hash[slot] = alloc;

	return 0;
}

static void free_seen_fsid(void)
{
	int slot;
	struct seen_fsid *seen;
	struct seen_fsid *next;

	for (slot = 0; slot < SEEN_FSID_HASH_SIZE; slot++) {
		seen = seen_fsid_hash[slot];
		while (seen) {
			next = seen->next;
			free(seen);
			seen = next;
		}
		seen_fsid_hash[slot] = NULL;
	}
}

static const char * const filesystem_cmd_group_usage[] = {
	"btrfs filesystem [<group>] <command> [<args>]",
	NULL
};

static const char * const cmd_filesystem_df_usage[] = {
	"btrfs filesystem df [options] <path>",
	"Show space usage information for a mount point",
	HELPINFO_UNITS_SHORT_LONG,
	NULL
};

static int get_df(int fd, struct btrfs_ioctl_space_args **sargs_ret)
{
	u64 count = 0;
	int ret;
	struct btrfs_ioctl_space_args *sargs;

	sargs = malloc(sizeof(struct btrfs_ioctl_space_args));
	if (!sargs)
		return -ENOMEM;

	sargs->space_slots = 0;
	sargs->total_spaces = 0;

	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	if (ret < 0) {
		error("cannot get space info: %s\n", strerror(errno));
		free(sargs);
		return -errno;
	}
	/* This really should never happen */
	if (!sargs->total_spaces) {
		free(sargs);
		return -ENOENT;
	}
	count = sargs->total_spaces;
	free(sargs);

	sargs = malloc(sizeof(struct btrfs_ioctl_space_args) +
			(count * sizeof(struct btrfs_ioctl_space_info)));
	if (!sargs)
		return -ENOMEM;

	sargs->space_slots = count;
	sargs->total_spaces = 0;
	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	if (ret < 0) {
		error("cannot get space info with %llu slots: %s",
				count, strerror(errno));
		free(sargs);
		return -errno;
	}
	*sargs_ret = sargs;
	return 0;
}

static void print_df(struct btrfs_ioctl_space_args *sargs, unsigned unit_mode)
{
	u64 i;
	struct btrfs_ioctl_space_info *sp = sargs->spaces;

	for (i = 0; i < sargs->total_spaces; i++, sp++) {
		printf("%s, %s: total=%s, used=%s\n",
			btrfs_group_type_str(sp->flags),
			btrfs_group_profile_str(sp->flags),
			pretty_size_mode(sp->total_bytes, unit_mode),
			pretty_size_mode(sp->used_bytes, unit_mode));
	}
}

static int cmd_filesystem_df(int argc, char **argv)
{
	struct btrfs_ioctl_space_args *sargs = NULL;
	int ret;
	int fd;
	char *path;
	DIR *dirstream = NULL;
	unsigned unit_mode;

	unit_mode = get_unit_mode_from_arg(&argc, argv, 1);

	clean_args_no_options(argc, argv, cmd_filesystem_df_usage);

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_filesystem_df_usage);

	path = argv[optind];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = get_df(fd, &sargs);

	if (ret == 0) {
		print_df(sargs, unit_mode);
		free(sargs);
	} else {
		error("get_df failed %s", strerror(-ret));
	}

	close_file_or_dir(fd, dirstream);
	return !!ret;
}

static int match_search_item_kernel(__u8 *fsid, char *mnt, char *label,
					char *search)
{
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	int search_len = strlen(search);

	search_len = min(search_len, BTRFS_UUID_UNPARSED_SIZE);
	uuid_unparse(fsid, uuidbuf);
	if (!strncmp(uuidbuf, search, search_len))
		return 1;

	if (*label && strcmp(label, search) == 0)
		return 1;

	if (strcmp(mnt, search) == 0)
		return 1;

	return 0;
}

static int uuid_search(struct btrfs_fs_devices *fs_devices, char *search)
{
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	struct list_head *cur;
	struct btrfs_device *device;
	int search_len = strlen(search);

	search_len = min(search_len, BTRFS_UUID_UNPARSED_SIZE);
	uuid_unparse(fs_devices->fsid, uuidbuf);
	if (!strncmp(uuidbuf, search, search_len))
		return 1;

	list_for_each(cur, &fs_devices->devices) {
		device = list_entry(cur, struct btrfs_device, dev_list);
		if ((device->label && strcmp(device->label, search) == 0) ||
		    strcmp(device->name, search) == 0)
			return 1;
	}
	return 0;
}

/*
 * Sort devices by devid, ascending
 */
static int cmp_device_id(void *priv, struct list_head *a,
		struct list_head *b)
{
	const struct btrfs_device *da = list_entry(a, struct btrfs_device,
			dev_list);
	const struct btrfs_device *db = list_entry(b, struct btrfs_device,
			dev_list);

	return da->devid < db->devid ? -1 :
		da->devid > db->devid ? 1 : 0;
}

static void splice_device_list(struct list_head *seed_devices,
			       struct list_head *all_devices)
{
	struct btrfs_device *in_all, *next_all;
	struct btrfs_device *in_seed, *next_seed;

	list_for_each_entry_safe(in_all, next_all, all_devices, dev_list) {
		list_for_each_entry_safe(in_seed, next_seed, seed_devices,
								dev_list) {
			if (in_all->devid == in_seed->devid) {
				/*
				 * When do dev replace in a sprout fs
				 * to a dev in its seed fs, the replacing
				 * dev will reside in the sprout fs and
				 * the replaced dev will still exist
				 * in the seed fs.
				 * So pick the latest one when showing
				 * the sprout fs.
				 */
				if (in_all->generation
						< in_seed->generation) {
					list_del(&in_all->dev_list);
					free(in_all);
				} else if (in_all->generation
						> in_seed->generation) {
					list_del(&in_seed->dev_list);
					free(in_seed);
				}
				break;
			}
		}
	}

	list_splice(seed_devices, all_devices);
}

static void print_devices(struct btrfs_fs_devices *fs_devices,
			  u64 *devs_found, unsigned unit_mode)
{
	struct btrfs_device *device;
	struct btrfs_fs_devices *cur_fs;
	struct list_head *all_devices;

	all_devices = &fs_devices->devices;
	cur_fs = fs_devices->seed;
	/* add all devices of seed fs to the fs to be printed */
	while (cur_fs) {
		splice_device_list(&cur_fs->devices, all_devices);
		cur_fs = cur_fs->seed;
	}

	list_sort(NULL, all_devices, cmp_device_id);
	list_for_each_entry(device, all_devices, dev_list) {
		printf("\tdevid %4llu size %s used %s path %s\n",
		       (unsigned long long)device->devid,
		       pretty_size_mode(device->total_bytes, unit_mode),
		       pretty_size_mode(device->bytes_used, unit_mode),
		       device->name);

		(*devs_found)++;
	}
}

static void print_one_uuid(struct btrfs_fs_devices *fs_devices,
			   unsigned unit_mode)
{
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	struct btrfs_device *device;
	u64 devs_found = 0;
	u64 total;

	if (add_seen_fsid(fs_devices->fsid))
		return;

	uuid_unparse(fs_devices->fsid, uuidbuf);
	device = list_entry(fs_devices->devices.next, struct btrfs_device,
			    dev_list);
	if (device->label && device->label[0])
		printf("Label: '%s' ", device->label);
	else
		printf("Label: none ");

	total = device->total_devs;
	printf(" uuid: %s\n\tTotal devices %llu FS bytes used %s\n", uuidbuf,
	       (unsigned long long)total,
	       pretty_size_mode(device->super_bytes_used, unit_mode));

	print_devices(fs_devices, &devs_found, unit_mode);

	if (devs_found < total) {
		printf("\t*** Some devices missing\n");
	}
	printf("\n");
}

/* adds up all the used spaces as reported by the space info ioctl
 */
static u64 calc_used_bytes(struct btrfs_ioctl_space_args *si)
{
	u64 ret = 0;
	int i;
	for (i = 0; i < si->total_spaces; i++)
		ret += si->spaces[i].used_bytes;
	return ret;
}

static int print_one_fs(struct btrfs_ioctl_fs_info_args *fs_info,
		struct btrfs_ioctl_dev_info_args *dev_info,
		struct btrfs_ioctl_space_args *space_info,
		char *label, unsigned unit_mode)
{
	int i;
	int fd;
	int missing = 0;
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	struct btrfs_ioctl_dev_info_args *tmp_dev_info;
	int ret;

	ret = add_seen_fsid(fs_info->fsid);
	if (ret == -EEXIST)
		return 0;
	else if (ret)
		return ret;

	uuid_unparse(fs_info->fsid, uuidbuf);
	if (label && *label)
		printf("Label: '%s' ", label);
	else
		printf("Label: none ");

	printf(" uuid: %s\n\tTotal devices %llu FS bytes used %s\n", uuidbuf,
			fs_info->num_devices,
			pretty_size_mode(calc_used_bytes(space_info),
					 unit_mode));

	for (i = 0; i < fs_info->num_devices; i++) {
		char *canonical_path;

		tmp_dev_info = (struct btrfs_ioctl_dev_info_args *)&dev_info[i];

		/* Add check for missing devices even mounted */
		fd = open((char *)tmp_dev_info->path, O_RDONLY);
		if (fd < 0) {
			missing = 1;
			continue;
		}
		close(fd);
		canonical_path = canonicalize_path((char *)tmp_dev_info->path);
		printf("\tdevid %4llu size %s used %s path %s\n",
			tmp_dev_info->devid,
			pretty_size_mode(tmp_dev_info->total_bytes, unit_mode),
			pretty_size_mode(tmp_dev_info->bytes_used, unit_mode),
			canonical_path);

		free(canonical_path);
	}

	if (missing)
		printf("\t*** Some devices missing\n");
	printf("\n");
	return 0;
}

static int btrfs_scan_kernel(void *search, unsigned unit_mode)
{
	int ret = 0, fd;
	int found = 0;
	FILE *f;
	struct mntent *mnt;
	struct btrfs_ioctl_fs_info_args fs_info_arg;
	struct btrfs_ioctl_dev_info_args *dev_info_arg = NULL;
	struct btrfs_ioctl_space_args *space_info_arg = NULL;
	char label[BTRFS_LABEL_SIZE];

	f = setmntent("/proc/self/mounts", "r");
	if (f == NULL)
		return 1;

	memset(label, 0, sizeof(label));
	while ((mnt = getmntent(f)) != NULL) {
		free(dev_info_arg);
		dev_info_arg = NULL;
		if (strcmp(mnt->mnt_type, "btrfs"))
			continue;
		ret = get_fs_info(mnt->mnt_dir, &fs_info_arg,
				&dev_info_arg);
		if (ret)
			goto out;

		/* skip all fs already shown as mounted fs */
		if (is_seen_fsid(fs_info_arg.fsid))
			continue;

		ret = get_label_mounted(mnt->mnt_dir, label);
		/* provide backward kernel compatibility */
		if (ret == -ENOTTY)
			ret = get_label_unmounted(
				(const char *)dev_info_arg->path, label);

		if (ret)
			goto out;

		if (search && !match_search_item_kernel(fs_info_arg.fsid,
					mnt->mnt_dir, label, search)) {
			continue;
		}

		fd = open(mnt->mnt_dir, O_RDONLY);
		if ((fd != -1) && !get_df(fd, &space_info_arg)) {
			print_one_fs(&fs_info_arg, dev_info_arg,
				     space_info_arg, label, unit_mode);
			kfree(space_info_arg);
			memset(label, 0, sizeof(label));
			found = 1;
		}
		if (fd != -1)
			close(fd);
	}

out:
	free(dev_info_arg);
	endmntent(f);
	return !found;
}

static int dev_to_fsid(char *dev, __u8 *fsid)
{
	struct btrfs_super_block *disk_super;
	char buf[BTRFS_SUPER_INFO_SIZE];
	int ret;
	int fd;

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		return ret;
	}

	disk_super = (struct btrfs_super_block *)buf;
	ret = btrfs_read_dev_super(fd, disk_super,
				   BTRFS_SUPER_INFO_OFFSET, 0);
	if (ret)
		goto out;

	memcpy(fsid, disk_super->fsid, BTRFS_FSID_SIZE);
	ret = 0;

out:
	close(fd);
	return ret;
}

static void free_fs_devices(struct btrfs_fs_devices *fs_devices)
{
	struct btrfs_fs_devices *cur_seed, *next_seed;
	struct btrfs_device *device;

	while (!list_empty(&fs_devices->devices)) {
		device = list_entry(fs_devices->devices.next,
					struct btrfs_device, dev_list);
		list_del(&device->dev_list);

		free(device->name);
		free(device->label);
		free(device);
	}

	/* free seed fs chain */
	cur_seed = fs_devices->seed;
	fs_devices->seed = NULL;
	while (cur_seed) {
		next_seed = cur_seed->seed;
		free(cur_seed);

		cur_seed = next_seed;
	}

	list_del(&fs_devices->list);
	free(fs_devices);
}

static int copy_device(struct btrfs_device *dst,
		       struct btrfs_device *src)
{
	dst->devid = src->devid;
	memcpy(dst->uuid, src->uuid, BTRFS_UUID_SIZE);
	if (src->name == NULL)
		dst->name = NULL;
	else {
		dst->name = strdup(src->name);
		if (!dst->name)
			return -ENOMEM;
	}
	if (src->label == NULL)
		dst->label = NULL;
	else {
		dst->label = strdup(src->label);
		if (!dst->label) {
			free(dst->name);
			return -ENOMEM;
		}
	}
	dst->total_devs = src->total_devs;
	dst->super_bytes_used = src->super_bytes_used;
	dst->total_bytes = src->total_bytes;
	dst->bytes_used = src->bytes_used;
	dst->generation = src->generation;

	return 0;
}

static int copy_fs_devices(struct btrfs_fs_devices *dst,
			   struct btrfs_fs_devices *src)
{
	struct btrfs_device *cur_dev, *dev_copy;
	int ret = 0;

	memcpy(dst->fsid, src->fsid, BTRFS_FSID_SIZE);
	INIT_LIST_HEAD(&dst->devices);
	dst->seed = NULL;

	list_for_each_entry(cur_dev, &src->devices, dev_list) {
		dev_copy = malloc(sizeof(*dev_copy));
		if (!dev_copy) {
			ret = -ENOMEM;
			break;
		}

		ret = copy_device(dev_copy, cur_dev);
		if (ret) {
			free(dev_copy);
			break;
		}

		list_add(&dev_copy->dev_list, &dst->devices);
		dev_copy->fs_devices = dst;
	}

	return ret;
}

static int find_and_copy_seed(struct btrfs_fs_devices *seed,
			      struct btrfs_fs_devices *copy,
			      struct list_head *fs_uuids) {
	struct btrfs_fs_devices *cur_fs;

	list_for_each_entry(cur_fs, fs_uuids, list)
		if (!memcmp(seed->fsid, cur_fs->fsid, BTRFS_FSID_SIZE))
			return copy_fs_devices(copy, cur_fs);

	return 1;
}

static int has_seed_devices(struct btrfs_fs_devices *fs_devices)
{
	struct btrfs_device *device;
	int dev_cnt_total, dev_cnt = 0;

	device = list_first_entry(&fs_devices->devices, struct btrfs_device,
				  dev_list);

	dev_cnt_total = device->total_devs;

	list_for_each_entry(device, &fs_devices->devices, dev_list)
		dev_cnt++;

	return dev_cnt_total != dev_cnt;
}

static int search_umounted_fs_uuids(struct list_head *all_uuids,
				    char *search, int *found)
{
	struct btrfs_fs_devices *cur_fs, *fs_copy;
	struct list_head *fs_uuids;
	int ret = 0;

	fs_uuids = btrfs_scanned_uuids();

	/*
	 * The fs_uuids list is global, and open_ctree_* will
	 * modify it, make a private copy here
	 */
	list_for_each_entry(cur_fs, fs_uuids, list) {
		/* don't bother handle all fs, if search target specified */
		if (search) {
			if (uuid_search(cur_fs, search) == 0)
				continue;
			if (found)
				*found = 1;
		}

		/* skip all fs already shown as mounted fs */
		if (is_seen_fsid(cur_fs->fsid))
			continue;

		fs_copy = calloc(1, sizeof(*fs_copy));
		if (!fs_copy) {
			ret = -ENOMEM;
			goto out;
		}

		ret = copy_fs_devices(fs_copy, cur_fs);
		if (ret) {
			free(fs_copy);
			goto out;
		}

		list_add(&fs_copy->list, all_uuids);
	}

out:
	return ret;
}

static int map_seed_devices(struct list_head *all_uuids)
{
	struct btrfs_fs_devices *cur_fs, *cur_seed;
	struct btrfs_fs_devices *seed_copy;
	struct btrfs_fs_devices *opened_fs;
	struct btrfs_device *device;
	struct btrfs_fs_info *fs_info;
	struct list_head *fs_uuids;
	int ret = 0;

	fs_uuids = btrfs_scanned_uuids();

	list_for_each_entry(cur_fs, all_uuids, list) {
		device = list_first_entry(&cur_fs->devices,
						struct btrfs_device, dev_list);
		if (!device)
			continue;

		/* skip fs without seeds */
		if (!has_seed_devices(cur_fs))
			continue;

		/*
		 * open_ctree_* detects seed/sprout mapping
		 */
		fs_info = open_ctree_fs_info(device->name, 0, 0, 0,
						OPEN_CTREE_PARTIAL);
		if (!fs_info)
			continue;

		/*
		 * copy the seed chain under the opened fs
		 */
		opened_fs = fs_info->fs_devices;
		cur_seed = cur_fs;
		while (opened_fs->seed) {
			seed_copy = malloc(sizeof(*seed_copy));
			if (!seed_copy) {
				ret = -ENOMEM;
				goto fail_out;
			}
			ret = find_and_copy_seed(opened_fs->seed, seed_copy,
						 fs_uuids);
			if (ret) {
				free(seed_copy);
				goto fail_out;
			}

			cur_seed->seed = seed_copy;

			opened_fs = opened_fs->seed;
			cur_seed = cur_seed->seed;
		}

		close_ctree(fs_info->chunk_root);
	}

out:
	return ret;
fail_out:
	close_ctree(fs_info->chunk_root);
	goto out;
}

static const char * const cmd_filesystem_show_usage[] = {
	"btrfs filesystem show [options] [<path>|<uuid>|<device>|label]",
	"Show the structure of a filesystem",
	"-d|--all-devices   show only disks under /dev containing btrfs filesystem",
	"-m|--mounted       show only mounted btrfs",
	HELPINFO_UNITS_LONG,
	"If no argument is given, structure of all present filesystems is shown.",
	NULL
};

static int cmd_filesystem_show(int argc, char **argv)
{
	LIST_HEAD(all_uuids);
	struct btrfs_fs_devices *fs_devices;
	char *search = NULL;
	int ret;
	/* default, search both kernel and udev */
	int where = -1;
	int type = 0;
	char mp[PATH_MAX];
	char path[PATH_MAX];
	__u8 fsid[BTRFS_FSID_SIZE];
	char uuid_buf[BTRFS_UUID_UNPARSED_SIZE];
	unsigned unit_mode;
	int found = 0;

	unit_mode = get_unit_mode_from_arg(&argc, argv, 0);

	while (1) {
		int c;
		static const struct option long_options[] = {
			{ "all-devices", no_argument, NULL, 'd'},
			{ "mounted", no_argument, NULL, 'm'},
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "dm", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'd':
			where = BTRFS_SCAN_LBLKID;
			break;
		case 'm':
			where = BTRFS_SCAN_MOUNTED;
			break;
		default:
			usage(cmd_filesystem_show_usage);
		}
	}

	if (check_argc_max(argc, optind + 1))
		usage(cmd_filesystem_show_usage);

	if (argc > optind) {
		search = argv[optind];
		if (*search == 0)
			usage(cmd_filesystem_show_usage);
		type = check_arg_type(search);

		/*
		 * For search is a device:
		 *     realpath do /dev/mapper/XX => /dev/dm-X
		 *     which is required by BTRFS_SCAN_DEV
		 * For search is a mountpoint:
		 *     realpath do  /mnt/btrfs/  => /mnt/btrfs
		 *     which shall be recognized by btrfs_scan_kernel()
		 */
		if (realpath(search, path))
			search = path;

		/*
		 * Needs special handling if input arg is block dev And if
		 * input arg is mount-point just print it right away
		 */
		if (type == BTRFS_ARG_BLKDEV && where != BTRFS_SCAN_LBLKID) {
			ret = get_btrfs_mount(search, mp, sizeof(mp));
			if (!ret) {
				/* given block dev is mounted */
				search = mp;
				type = BTRFS_ARG_MNTPOINT;
			} else {
				ret = dev_to_fsid(search, fsid);
				if (ret) {
					error("no btrfs on %s", search);
					return 1;
				}
				uuid_unparse(fsid, uuid_buf);
				search = uuid_buf;
				type = BTRFS_ARG_UUID;
				goto devs_only;
			}
		}
	}

	if (where == BTRFS_SCAN_LBLKID)
		goto devs_only;

	/* show mounted btrfs */
	ret = btrfs_scan_kernel(search, unit_mode);
	if (search && !ret) {
		/* since search is found we are done */
		goto out;
	}

	/* shows mounted only */
	if (where == BTRFS_SCAN_MOUNTED)
		goto out;

devs_only:
	ret = btrfs_scan_lblkid();

	if (ret) {
		error("blkid device scan returned %d\n", ret);
		return 1;
	}

	ret = search_umounted_fs_uuids(&all_uuids, search, &found);
	if (ret < 0) {
		error("searching target device returned error %d", ret);
		return 1;
	}

	/*
	 * The seed/sprout mapping are not detected yet,
	 * do mapping build for all umounted fs
	 */
	ret = map_seed_devices(&all_uuids);
	if (ret) {
		error("mapping seed devices returned error %d", ret);
		return 1;
	}

	list_for_each_entry(fs_devices, &all_uuids, list)
		print_one_uuid(fs_devices, unit_mode);

	if (search && !found)
		ret = 1;

	while (!list_empty(&all_uuids)) {
		fs_devices = list_entry(all_uuids.next,
					struct btrfs_fs_devices, list);
		free_fs_devices(fs_devices);
	}
out:
	free_seen_fsid();
	return ret;
}

static const char * const cmd_filesystem_sync_usage[] = {
	"btrfs filesystem sync <path>",
	"Force a sync on a filesystem",
	NULL
};

static int cmd_filesystem_sync(int argc, char **argv)
{
	int 	fd, res, e;
	char	*path;
	DIR	*dirstream = NULL;

	clean_args_no_options(argc, argv, cmd_filesystem_sync_usage);

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_filesystem_sync_usage);

	path = argv[optind];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	printf("FSSync '%s'\n", path);
	res = ioctl(fd, BTRFS_IOC_SYNC);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if( res < 0 ){
		error("sync ioctl failed on '%s': %s", path, strerror(e));
		return 1;
	}

	return 0;
}

static int parse_compress_type(char *s)
{
	if (strcmp(optarg, "zlib") == 0)
		return BTRFS_COMPRESS_ZLIB;
	else if (strcmp(optarg, "lzo") == 0)
		return BTRFS_COMPRESS_LZO;
	else {
		error("unknown compression type %s", s);
		exit(1);
	};
}

static const char * const cmd_filesystem_defrag_usage[] = {
	"btrfs filesystem defragment [options] <file>|<dir> [<file>|<dir>...]",
	"Defragment a file or a directory",
	"",
	"-v             be verbose",
	"-r             defragment files recursively",
	"-c[zlib,lzo]   compress the file while defragmenting",
	"-f             flush data to disk immediately after defragmenting",
	"-s start       defragment only from byte onward",
	"-l len         defragment only up to len bytes",
	"-t size        target extent size hint",
	NULL
};

static int do_defrag(int fd, int fancy_ioctl,
		struct btrfs_ioctl_defrag_range_args *range)
{
	int ret;

	if (!fancy_ioctl)
		ret = ioctl(fd, BTRFS_IOC_DEFRAG, NULL);
	else
		ret = ioctl(fd, BTRFS_IOC_DEFRAG_RANGE, range);

	return ret;
}

static int defrag_global_fancy_ioctl;
static struct btrfs_ioctl_defrag_range_args defrag_global_range;
static int defrag_global_verbose;
static int defrag_global_errors;
static int defrag_callback(const char *fpath, const struct stat *sb,
		int typeflag, struct FTW *ftwbuf)
{
	int ret = 0;
	int e = 0;
	int fd = 0;

	if ((typeflag == FTW_F) && S_ISREG(sb->st_mode)) {
		if (defrag_global_verbose)
			printf("%s\n", fpath);
		fd = open(fpath, O_RDWR);
		if (fd < 0)
			goto error;
		ret = do_defrag(fd, defrag_global_fancy_ioctl, &defrag_global_range);
		e = errno;
		close(fd);
		if (ret && e == ENOTTY && defrag_global_fancy_ioctl) {
			error("defrag range ioctl not "
				"supported in this kernel, please try "
				"without any options.");
			defrag_global_errors++;
			return ENOTTY;
		}
		if (ret)
			goto error;
	}
	return 0;

error:
	error("defrag failed on %s: %s", fpath, strerror(e));
	defrag_global_errors++;
	return 0;
}

static int cmd_filesystem_defrag(int argc, char **argv)
{
	int fd;
	int flush = 0;
	u64 start = 0;
	u64 len = (u64)-1;
	u64 thresh = 0;
	int i;
	int recursive = 0;
	int ret = 0;
	int e = 0;
	int compress_type = BTRFS_COMPRESS_NONE;
	DIR *dirstream;

	defrag_global_errors = 0;
	defrag_global_verbose = 0;
	defrag_global_errors = 0;
	defrag_global_fancy_ioctl = 0;
	optind = 1;
	while(1) {
		int c = getopt(argc, argv, "vrc::fs:l:t:");
		if (c < 0)
			break;

		switch(c) {
		case 'c':
			compress_type = BTRFS_COMPRESS_ZLIB;
			if (optarg)
				compress_type = parse_compress_type(optarg);
			defrag_global_fancy_ioctl = 1;
			break;
		case 'f':
			flush = 1;
			defrag_global_fancy_ioctl = 1;
			break;
		case 'v':
			defrag_global_verbose = 1;
			break;
		case 's':
			start = parse_size(optarg);
			defrag_global_fancy_ioctl = 1;
			break;
		case 'l':
			len = parse_size(optarg);
			defrag_global_fancy_ioctl = 1;
			break;
		case 't':
			thresh = parse_size(optarg);
			if (thresh > (u32)-1) {
				warning(
			    "target extent size %llu too big, trimmed to %u",
					thresh, (u32)-1);
				thresh = (u32)-1;
			}
			defrag_global_fancy_ioctl = 1;
			break;
		case 'r':
			recursive = 1;
			break;
		default:
			usage(cmd_filesystem_defrag_usage);
		}
	}

	if (check_argc_min(argc - optind, 1))
		usage(cmd_filesystem_defrag_usage);

	memset(&defrag_global_range, 0, sizeof(defrag_global_range));
	defrag_global_range.start = start;
	defrag_global_range.len = len;
	defrag_global_range.extent_thresh = (u32)thresh;
	if (compress_type) {
		defrag_global_range.flags |= BTRFS_DEFRAG_RANGE_COMPRESS;
		defrag_global_range.compress_type = compress_type;
	}
	if (flush)
		defrag_global_range.flags |= BTRFS_DEFRAG_RANGE_START_IO;

	for (i = optind; i < argc; i++) {
		struct stat st;

		dirstream = NULL;
		fd = open_file_or_dir(argv[i], &dirstream);
		if (fd < 0) {
			error("cannot open %s: %s\n", argv[i],
					strerror(errno));
			defrag_global_errors++;
			close_file_or_dir(fd, dirstream);
			continue;
		}
		if (fstat(fd, &st)) {
			error("failed to stat %s: %s",
					argv[i], strerror(errno));
			defrag_global_errors++;
			close_file_or_dir(fd, dirstream);
			continue;
		}
		if (!(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) {
			error("%s is not a directory or a regular file\n",
					argv[i]);
			defrag_global_errors++;
			close_file_or_dir(fd, dirstream);
			continue;
		}
		if (recursive) {
			if (S_ISDIR(st.st_mode)) {
				ret = nftw(argv[i], defrag_callback, 10,
						FTW_MOUNT | FTW_PHYS);
				if (ret == ENOTTY)
					exit(1);
				/* errors are handled in the callback */
				ret = 0;
			} else {
				if (defrag_global_verbose)
					printf("%s\n", argv[i]);
				ret = do_defrag(fd, defrag_global_fancy_ioctl,
						&defrag_global_range);
				e = errno;
			}
		} else {
			if (defrag_global_verbose)
				printf("%s\n", argv[i]);
			ret = do_defrag(fd, defrag_global_fancy_ioctl,
					&defrag_global_range);
			e = errno;
		}
		close_file_or_dir(fd, dirstream);
		if (ret && e == ENOTTY && defrag_global_fancy_ioctl) {
			error("defrag range ioctl not "
				"supported in this kernel, please try "
				"without any options.");
			defrag_global_errors++;
			break;
		}
		if (ret) {
			error("defrag failed on %s: %s", argv[i], strerror(e));
			defrag_global_errors++;
		}
	}
	if (defrag_global_errors)
		fprintf(stderr, "total %d failures\n", defrag_global_errors);

	return !!defrag_global_errors;
}

static const char * const cmd_filesystem_resize_usage[] = {
	"btrfs filesystem resize [devid:][+/-]<newsize>[kKmMgGtTpPeE]|[devid:]max <path>",
	"Resize a filesystem",
	"If 'max' is passed, the filesystem will occupy all available space",
	"on the device 'devid'.",
	"[kK] means KiB, which denotes 1KiB = 1024B, 1MiB = 1024KiB, etc.",
	NULL
};

static int cmd_filesystem_resize(int argc, char **argv)
{
	struct btrfs_ioctl_vol_args	args;
	int	fd, res, len, e;
	char	*amount, *path;
	DIR	*dirstream = NULL;
	struct stat st;

	clean_args_no_options(argc, argv, cmd_filesystem_resize_usage);

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_filesystem_resize_usage);

	amount = argv[optind];
	path = argv[optind + 1];

	len = strlen(amount);
	if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
		error("resize value too long (%s)", amount);
		return 1;
	}

	res = stat(path, &st);
	if (res < 0) {
		error("resize: cannot stat %s: %s", path, strerror(errno));
		return 1;
	}
	if (!S_ISDIR(st.st_mode)) {
		error("resize works on mounted filesystems and accepts only\n"
			"directories as argument. Passing file containing a btrfs image\n"
			"would resize the underlying filesystem instead of the image.\n");
		return 1;
	}

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	printf("Resize '%s' of '%s'\n", path, amount);
	memset(&args, 0, sizeof(args));
	strncpy_null(args.name, amount);
	res = ioctl(fd, BTRFS_IOC_RESIZE, &args);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if( res < 0 ){
		switch (e) {
		case EFBIG:
			error("unable to resize '%s': no enough free space",
				path);
			break;
		default:
			error("unable to resize '%s': %s", path, strerror(e));
			break;
		}
		return 1;
	} else if (res > 0) {
		const char *err_str = btrfs_err_str(res);

		if (err_str) {
			error("resizing of '%s' failed: %s", path, err_str);
		} else {
			error("resizing of '%s' failed: unknown error %d",
				path, res);
		}
		return 1;
	}
	return 0;
}

static const char * const cmd_filesystem_label_usage[] = {
	"btrfs filesystem label [<device>|<mount_point>] [<newlabel>]",
	"Get or change the label of a filesystem",
	"With one argument, get the label of filesystem on <device>.",
	"If <newlabel> is passed, set the filesystem label to <newlabel>.",
	NULL
};

static int cmd_filesystem_label(int argc, char **argv)
{
	clean_args_no_options(argc, argv, cmd_filesystem_label_usage);

	if (check_argc_min(argc - optind, 1) ||
			check_argc_max(argc - optind, 2))
		usage(cmd_filesystem_label_usage);

	if (argc - optind > 1) {
		return set_label(argv[optind], argv[optind + 1]);
	} else {
		char label[BTRFS_LABEL_SIZE];
		int ret;

		ret = get_label(argv[optind], label);
		if (!ret)
			fprintf(stdout, "%s\n", label);

		return ret;
	}
}

static const char filesystem_cmd_group_info[] =
"overall filesystem tasks and information";

const struct cmd_group filesystem_cmd_group = {
	filesystem_cmd_group_usage, filesystem_cmd_group_info, {
		{ "df", cmd_filesystem_df, cmd_filesystem_df_usage, NULL, 0 },
		{ "du", cmd_filesystem_du, cmd_filesystem_du_usage, NULL, 0 },
		{ "show", cmd_filesystem_show, cmd_filesystem_show_usage, NULL,
			0 },
		{ "sync", cmd_filesystem_sync, cmd_filesystem_sync_usage, NULL,
			0 },
		{ "defragment", cmd_filesystem_defrag,
			cmd_filesystem_defrag_usage, NULL, 0 },
		{ "balance", cmd_balance, NULL, &balance_cmd_group,
			CMD_HIDDEN },
		{ "resize", cmd_filesystem_resize, cmd_filesystem_resize_usage,
			NULL, 0 },
		{ "label", cmd_filesystem_label, cmd_filesystem_label_usage,
			NULL, 0 },
		{ "usage", cmd_filesystem_usage,
			cmd_filesystem_usage_usage, NULL, 0 },

		NULL_CMD_STRUCT
	}
};

int cmd_filesystem(int argc, char **argv)
{
	return handle_command_group(&filesystem_cmd_group, argc, argv);
}
