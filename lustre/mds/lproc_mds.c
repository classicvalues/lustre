/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 *
 */
#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/version.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
#include <asm/statfs.h>
#endif
#include <linux/obd.h>
#include <linux/obd_class.h>
#include <linux/lprocfs_status.h>

#include "mds_internal.h"

#ifdef LPROCFS
static int lprocfs_mds_rd_mntdev(char *page, char **start, off_t off, int count,
                                 int *eof, void *data)
{
        struct obd_device* obd = (struct obd_device *)data;

        LASSERT(obd != NULL);
        LASSERT(obd->u.mds.mds_vfsmnt->mnt_devname);
        *eof = 1;

        return snprintf(page, count, "%s\n",obd->u.mds.mds_vfsmnt->mnt_devname);
}

struct lprocfs_vars lprocfs_mds_obd_vars[] = {
        { "uuid",         lprocfs_rd_uuid,        0, 0 },
        { "blocksize",    lprocfs_rd_blksize,     0, 0 },
        { "kbytestotal",  lprocfs_rd_kbytestotal, 0, 0 },
        { "kbytesfree",   lprocfs_rd_kbytesfree,  0, 0 },
        { "kbytesavail",  lprocfs_rd_kbytesavail, 0, 0 },
        { "filestotal",   lprocfs_rd_filestotal,  0, 0 },
        { "filesfree",    lprocfs_rd_filesfree,   0, 0 },
        { "fstype",       lprocfs_rd_fstype,      0, 0 },
        { "mntdev",       lprocfs_mds_rd_mntdev,  0, 0 },
        { "recovery_status", lprocfs_obd_rd_recovery_status, 0, 0 },
        { "evict_client", 0, lprocfs_wr_evict_client, 0 },
        { "num_exports",  lprocfs_rd_num_exports, 0, 0 },
#ifdef HAVE_QUOTA_SUPPORT
        { "quota_bunit_sz", lprocfs_mds_rd_bunit, lprocfs_mds_wr_bunit, 0 },
        { "quota_btune_sz", lprocfs_mds_rd_btune, lprocfs_mds_wr_btune, 0 },
        { "quota_iunit_sz", lprocfs_mds_rd_iunit, lprocfs_mds_wr_iunit, 0 },
        { "quota_itune_sz", lprocfs_mds_rd_itune, lprocfs_mds_wr_itune, 0 },
#endif
        { 0 }
};

struct lprocfs_vars lprocfs_mds_module_vars[] = {
        { "num_refs",     lprocfs_rd_numrefs,     0, 0 },
        { 0 }
};

struct lprocfs_vars lprocfs_mdt_obd_vars[] = {
        { "uuid",         lprocfs_rd_uuid,        0, 0 },
        { 0 }
};

struct lprocfs_vars lprocfs_mdt_module_vars[] = {
        { "num_refs",     lprocfs_rd_numrefs,     0, 0 },
        { 0 }
};

LPROCFS_INIT_VARS(mds, lprocfs_mds_module_vars, lprocfs_mds_obd_vars);
LPROCFS_INIT_VARS(mdt, lprocfs_mdt_module_vars, lprocfs_mdt_obd_vars);
#endif
