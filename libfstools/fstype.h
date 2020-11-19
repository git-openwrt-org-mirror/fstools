/*
 * Copyright (C) 2014 John Crispin <blogic@openwrt.org>
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

#ifndef _FS_TYPE_H__
#define _FS_TYPE_H__

enum {
	FS_NONE,
	FS_SNAPSHOT,
	FS_JFFS2,
	FS_DEADCODE,
	FS_UBIFS,
	FS_F2FS,
	FS_EXT4,
};

#endif
