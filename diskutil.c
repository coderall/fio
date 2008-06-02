#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <libgen.h>
#include <math.h>

#include "fio.h"

static int last_majdev, last_mindev;

static struct flist_head disk_list = FLIST_HEAD_INIT(disk_list);

static int get_io_ticks(struct disk_util *du, struct disk_util_stat *dus)
{
	unsigned in_flight;
	char line[256];
	FILE *f;
	char *p;
	int ret;

	dprint(FD_DISKUTIL, "open stat file: %s\n", du->path);

	f = fopen(du->path, "r");
	if (!f)
		return 1;

	p = fgets(line, sizeof(line), f);
	if (!p) {
		fclose(f);
		return 1;
	}

	dprint(FD_DISKUTIL, "%s: %s", du->path, p);

	ret = sscanf(p, "%u %u %llu %u %u %u %llu %u %u %u %u\n", &dus->ios[0],
					&dus->merges[0], &dus->sectors[0],
					&dus->ticks[0], &dus->ios[1],
					&dus->merges[1], &dus->sectors[1],
					&dus->ticks[1], &in_flight,
					&dus->io_ticks, &dus->time_in_queue);
	fclose(f);
	dprint(FD_DISKUTIL, "%s: stat read ok? %d\n", du->path, ret == 1);
	return ret != 11;
}

static void update_io_tick_disk(struct disk_util *du)
{
	struct disk_util_stat __dus, *dus, *ldus;
	struct timeval t;

	if (get_io_ticks(du, &__dus))
		return;

	dus = &du->dus;
	ldus = &du->last_dus;

	dus->sectors[0] += (__dus.sectors[0] - ldus->sectors[0]);
	dus->sectors[1] += (__dus.sectors[1] - ldus->sectors[1]);
	dus->ios[0] += (__dus.ios[0] - ldus->ios[0]);
	dus->ios[1] += (__dus.ios[1] - ldus->ios[1]);
	dus->merges[0] += (__dus.merges[0] - ldus->merges[0]);
	dus->merges[1] += (__dus.merges[1] - ldus->merges[1]);
	dus->ticks[0] += (__dus.ticks[0] - ldus->ticks[0]);
	dus->ticks[1] += (__dus.ticks[1] - ldus->ticks[1]);
	dus->io_ticks += (__dus.io_ticks - ldus->io_ticks);
	dus->time_in_queue += (__dus.time_in_queue - ldus->time_in_queue);

	fio_gettime(&t, NULL);
	du->msec += mtime_since(&du->time, &t);
	memcpy(&du->time, &t, sizeof(t));
	memcpy(ldus, &__dus, sizeof(__dus));
}

void update_io_ticks(void)
{
	struct flist_head *entry;
	struct disk_util *du;

	dprint(FD_DISKUTIL, "update io ticks\n");

	flist_for_each(entry, &disk_list) {
		du = flist_entry(entry, struct disk_util, list);
		update_io_tick_disk(du);
	}
}

static struct disk_util *disk_util_exists(int major, int minor)
{
	struct flist_head *entry;
	struct disk_util *du;

	flist_for_each(entry, &disk_list) {
		du = flist_entry(entry, struct disk_util, list);

		if (major == du->major && minor == du->minor)
			return du;
	}

	return NULL;
}

static void disk_util_add(int majdev, int mindev, char *path)
{
	struct disk_util *du, *__du;
	struct flist_head *entry;

	dprint(FD_DISKUTIL, "add maj/min %d/%d: %s\n", majdev, mindev, path);

	du = malloc(sizeof(*du));
	memset(du, 0, sizeof(*du));
	INIT_FLIST_HEAD(&du->list);
	sprintf(du->path, "%s/stat", path);
	du->name = strdup(basename(path));
	du->sysfs_root = path;
	du->major = majdev;
	du->minor = mindev;

	flist_for_each(entry, &disk_list) {
		__du = flist_entry(entry, struct disk_util, list);

		dprint(FD_DISKUTIL, "found %s in list\n", __du->name);

		if (!strcmp(du->name, __du->name)) {
			free(du->name);
			free(du);
			return;
		}
	}

	dprint(FD_DISKUTIL, "add %s to list\n", du->name);

	fio_gettime(&du->time, NULL);
	get_io_ticks(du, &du->last_dus);

	flist_add_tail(&du->list, &disk_list);
}

static int check_dev_match(int majdev, int mindev, char *path)
{
	int major, minor;
	char line[256], *p;
	FILE *f;

	f = fopen(path, "r");
	if (!f) {
		perror("open path");
		return 1;
	}

	p = fgets(line, sizeof(line), f);
	if (!p) {
		fclose(f);
		return 1;
	}

	if (sscanf(p, "%u:%u", &major, &minor) != 2) {
		fclose(f);
		return 1;
	}

	if (majdev == major && mindev == minor) {
		fclose(f);
		return 0;
	}

	fclose(f);
	return 1;
}

static int find_block_dir(int majdev, int mindev, char *path)
{
	struct dirent *dir;
	struct stat st;
	int found = 0;
	DIR *D;

	D = opendir(path);
	if (!D)
		return 0;

	while ((dir = readdir(D)) != NULL) {
		char full_path[256];

		if (!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, ".."))
			continue;

		sprintf(full_path, "%s/%s", path, dir->d_name);

		if (!strcmp(dir->d_name, "dev")) {
			if (!check_dev_match(majdev, mindev, full_path)) {
				found = 1;
				break;
			}
		}

		if (lstat(full_path, &st) == -1) {
			perror("stat");
			break;
		}

		if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
			continue;

		found = find_block_dir(majdev, mindev, full_path);
		if (found) {
			strcpy(path, full_path);
			break;
		}
	}

	closedir(D);
	return found;
}

static void __init_disk_util(struct thread_data *td, struct fio_file *f)
{
	struct stat st;
	char foo[PATH_MAX], tmp[PATH_MAX];
	struct disk_util *du;
	int mindev, majdev;
	char *p;

	if (!lstat(f->file_name, &st)) {
		if (S_ISBLK(st.st_mode)) {
			majdev = major(st.st_rdev);
			mindev = minor(st.st_rdev);
		} else if (S_ISCHR(st.st_mode)) {
			majdev = major(st.st_rdev);
			mindev = minor(st.st_rdev);
			if (fio_lookup_raw(st.st_rdev, &majdev, &mindev))
				return;
		} else if (S_ISFIFO(st.st_mode))
			return;
		else {
			majdev = major(st.st_dev);
			mindev = minor(st.st_dev);
		}
	} else {
		/*
		 * must be a file, open "." in that path
		 */
		strncpy(foo, f->file_name, PATH_MAX - 1);
		p = dirname(foo);
		if (stat(p, &st)) {
			perror("disk util stat");
			return;
		}

		majdev = major(st.st_dev);
		mindev = minor(st.st_dev);
	}

	dprint(FD_DISKUTIL, "%s belongs to maj/min %d/%d\n", f->file_name,
							majdev, mindev);

	du = disk_util_exists(majdev, mindev);
	if (du) {
		if (td->o.ioscheduler && !td->sysfs_root)
			td->sysfs_root = strdup(du->sysfs_root);

		return;
	}

	/*
	 * for an fs without a device, we will repeatedly stat through
	 * sysfs which can take oodles of time for thousands of files. so
	 * cache the last lookup and compare with that before going through
	 * everything again.
	 */
	if (mindev == last_mindev && majdev == last_majdev)
		return;

	last_mindev = mindev;
	last_majdev = majdev;

	sprintf(foo, "/sys/block");
	if (!find_block_dir(majdev, mindev, foo))
		return;

	/*
	 * If there's a ../queue/ directory there, we are inside a partition.
	 * Check if that is the case and jump back. For loop/md/dm etc we
	 * are already in the right spot.
	 */
	sprintf(tmp, "%s/../queue", foo);
	if (!stat(tmp, &st)) {
		p = dirname(foo);
		sprintf(tmp, "%s/queue", p);
		if (stat(tmp, &st)) {
			log_err("unknown sysfs layout\n");
			return;
		}
		strncpy(tmp, p, PATH_MAX - 1);
		sprintf(foo, "%s", tmp);
	}

	if (td->o.ioscheduler && !td->sysfs_root)
		td->sysfs_root = strdup(foo);

	disk_util_add(majdev, mindev, foo);
}

void init_disk_util(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	if (!td->o.do_disk_util ||
	    (td->io_ops->flags & (FIO_DISKLESSIO | FIO_NODISKUTIL)))
		return;

	for_each_file(td, f, i)
		__init_disk_util(td, f);
}

void show_disk_util(void)
{
	struct disk_util_stat *dus;
	struct flist_head *entry, *next;
	struct disk_util *du;
	double util;

	if (flist_empty(&disk_list))
		return;

	log_info("\nDisk stats (read/write):\n");

	flist_for_each(entry, &disk_list) {
		du = flist_entry(entry, struct disk_util, list);
		dus = &du->dus;

		util = (double) 100 * du->dus.io_ticks / (double) du->msec;
		if (util > 100.0)
			util = 100.0;

		log_info("  %s: ios=%u/%u, merge=%u/%u, ticks=%u/%u, "
			 "in_queue=%u, util=%3.2f%%\n", du->name,
						dus->ios[0], dus->ios[1],
						dus->merges[0], dus->merges[1],
						dus->ticks[0], dus->ticks[1],
						dus->time_in_queue, util);
	}

	/*
	 * now free the list
	 */
	flist_for_each_safe(entry, next, &disk_list) {
		flist_del(entry);
		du = flist_entry(entry, struct disk_util, list);
		free(du->name);
		free(du);
	}
}
