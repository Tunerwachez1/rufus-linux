/*
 * Rufus Linux port - Linux HAL: device enumeration through libudev
 * GPLv3
 */
#include "linux_hal_internal.h"
#include "../core/log.h"

#include <ctype.h>
#include <libudev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static const char *prop_or(struct udev_device *dev, const char *prop, const char *fallback)
{
	const char *v = udev_device_get_property_value(dev, prop);
	return ((v != NULL) && (v[0] != '\0')) ? v : fallback;
}

static const char *attr_or(struct udev_device *dev, const char *attr, const char *fallback)
{
	const char *v = udev_device_get_sysattr_value(dev, attr);
	return ((v != NULL) && (v[0] != '\0')) ? v : fallback;
}

/* Virtual/ram devices that must never show up as write targets */
static bool is_virtual_device(const char *node)
{
	static const char *skip_prefix[] = {
		"loop", "ram", "zram", "dm-", "md", "nbd", "fd", "sr", "pmem", NULL
	};
	const char *base = strrchr(node, '/');

	base = base ? base + 1 : node;

	for (int i = 0; skip_prefix[i] != NULL; i++) {
		size_t len = strlen(skip_prefix[i]);
		if (strncmp(base, skip_prefix[i], len) == 0)
			return true;
	}
	return false;
}

static void fill_device(rfd_device *out, struct udev_device *dev)
{
	const char *node = udev_device_get_devnode(dev);
	const char *size_str;
	unsigned long long sectors = 0;

	memset(out, 0, sizeof(*out));
	if (node == NULL)
		return;
	snprintf(out->node, sizeof(out->node), "%s", node);

	snprintf(out->vendor, sizeof(out->vendor), "%s",
		attr_or(dev, "device/vendor",
			prop_or(dev, "ID_VENDOR_FROM_DATABASE", "")));
	snprintf(out->model, sizeof(out->model), "%s",
		attr_or(dev, "device/model",
			prop_or(dev, "ID_MODEL_FROM_DATABASE", "")));
	snprintf(out->serial, sizeof(out->serial), "%s",
		prop_or(dev, "ID_SERIAL_SHORT", ""));
	snprintf(out->bus, sizeof(out->bus), "%s",
		prop_or(dev, "ID_BUS", ""));

	out->usb = (strcmp(out->bus, "usb") == 0);
	out->removable = (strcmp(attr_or(dev, "removable", "0"), "1") == 0);
	out->readonly_device = (strcmp(attr_or(dev, "ro", "0"), "1") == 0);

	{
		unsigned long v = strtoul(
			attr_or(dev, "queue/logical_block_size", "512"),
			NULL, 10);

		/* sysfs data is external input: only sane powers of two */
		if ((v < 512) || (v > 65536) || ((v & (v - 1)) != 0))
			v = 512;
		out->sector_size = (uint32_t)v;
	}

	size_str = udev_device_get_sysattr_value(dev, "size");
	if (size_str != NULL)
		sectors = strtoull(size_str, NULL, 10);
	out->size_bytes = sectors * out->sector_size;
}

int rfd_linux_enumerate_devices(rfd_device *out, int max_count)
{
	struct udev *udev = NULL;
	struct udev_enumerate *enu = NULL;
	struct udev_list_entry *entries, *entry;
	int count = -1;

	udev = udev_new();
	if (udev == NULL) {
		rlog_err("could not create udev context");
		return -1;
	}

	enu = udev_enumerate_new(udev);
	if (enu == NULL)
		goto out;

	udev_enumerate_add_match_subsystem(enu, "block");
	udev_enumerate_add_match_property(enu, "DEVTYPE", "disk");
	udev_enumerate_scan_devices(enu);

	count = 0;
	entries = udev_enumerate_get_list_entry(enu);
	udev_list_entry_foreach(entry, entries) {
		const char *syspath = udev_list_entry_get_name(entry);
		struct udev_device *dev = udev_device_new_from_syspath(udev, syspath);
		const char *node;

		if (dev == NULL)
			continue;

		node = udev_device_get_devnode(dev);   /* may be NULL */
		if ((node != NULL) && !is_virtual_device(node)) {
			if (count >= max_count) {
				rlog_warn("too many devices, truncating list at %d", max_count);
				udev_device_unref(dev);
				break;
			}
			fill_device(&out[count], dev);
			count++;
		}
		udev_device_unref(dev);
	}

	udev_enumerate_unref(enu);
out:
	udev_unref(udev);
	return count;
}

/*
 * Collect the dev_t of every partition that belongs to the given disk.
 * Used by the CLI to refuse writing to a disk whose partitions are mounted.
 */
int rfd_linux_list_partitions(const char *node, dev_t *out, int max_count)
{
	struct udev *udev = NULL;
	struct udev_device *disk = NULL;
	struct udev_enumerate *enu = NULL;
	struct udev_list_entry *entries, *entry;
	struct stat st;
	int ret = -1;

	if (stat(node, &st) != 0)
		return -1;

	udev = udev_new();
	if (udev == NULL)
		return -1;

	disk = udev_device_new_from_devnum(udev, 'b', st.st_rdev);
	if (disk == NULL)
		goto out;

	enu = udev_enumerate_new(udev);
	if (enu == NULL)
		goto out;

	udev_enumerate_add_match_subsystem(enu, "block");
	udev_enumerate_add_match_parent(enu, disk);
	udev_enumerate_scan_devices(enu);

	ret = 0;
	entries = udev_enumerate_get_list_entry(enu);
	udev_list_entry_foreach(entry, entries) {
		const char *syspath = udev_list_entry_get_name(entry);
		struct udev_device *part = udev_device_new_from_syspath(udev, syspath);

		if (part == NULL)
			continue;

		if ((ret < max_count) &&
		    (strcmp(udev_device_get_devtype(part), "partition") == 0)) {
			const char *num = udev_device_get_property_value(part, "MAJOR");
			const char *min = udev_device_get_property_value(part, "MINOR");

			if ((num != NULL) && (min != NULL))
				out[ret++] = makedev(
					(unsigned)strtoul(num, NULL, 10),
					(unsigned)strtoul(min, NULL, 10));
		}
		udev_device_unref(part);
	}

	udev_enumerate_unref(enu);
out:
	udev_device_unref(disk);
	udev_unref(udev);
	return ret;
}
