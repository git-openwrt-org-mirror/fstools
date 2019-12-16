/*
 * Copyright (C) 2019 Anton Kikin <a.kikin@tano-systems.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "overlay_partition.h"

static char overlay_partition[64] = "";

static void __get_overlay_partition(void)
{
	FILE *fp;

	fp = fopen("/tmp/overlay_partition", "r");
	if (fp)
	{
		size_t n;
		n = fread(overlay_partition, 1, sizeof(overlay_partition) - 1, fp);

		if (!ferror(fp) && n)
		{
			/* trim readed data */
			for (--n; isspace(overlay_partition[n]) && (n >= 0); --n)
				overlay_partition[n] = 0;

			fclose(fp);
			return;
		}

		fclose(fp);
	}

	strcpy(overlay_partition, "rootfs_data");
}

char *get_overlay_partition(void)
{
	if (!overlay_partition[0])
		__get_overlay_partition();

	return overlay_partition;
}
