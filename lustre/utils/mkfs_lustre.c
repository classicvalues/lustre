/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2014, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/utils/mkfs_lustre.c
 *
 * Author: Nathan Rutman <nathan@clusterfs.com>
*/

/* This source file is compiled into both mkfs.lustre and tunefs.lustre */

#if HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "mount_utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <mntent.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/utsname.h>

#include <string.h>
#include <getopt.h>
#include <limits.h>
#include <ctype.h>
#include <lnet/nidstr.h>
#include <lustre_disk.h>
#include <lustre_param.h>
#include <lnet/lnetctl.h>
#include <lustre_ver.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

char *progname;
int verbose = 1;
int version;
static int print_only = 0;

#ifdef HAVE_LDISKFS_OSD
#define FSLIST_LDISKFS "ldiskfs"
#define HAVE_FSLIST
#else
 #define FSLIST_LDISKFS ""
#endif /* HAVE_LDISKFS_OSD */
#ifdef HAVE_ZFS_OSD
 #ifdef HAVE_FSLIST
   #define FSLIST_ZFS "|zfs"
 #else
  #define FSLIST_ZFS "zfs"
  #define HAVE_FSLIST
 #endif
#else
 #define FSLIST_ZFS ""
#endif /* HAVE_ZFS_OSD */

#ifndef HAVE_FSLIST
 #error "no backing OSD types (ldiskfs or ZFS) are configured"
#endif

#define FSLIST FSLIST_LDISKFS FSLIST_ZFS

void usage(FILE *out)
{
	fprintf(out, "usage: %s <target type> [--backfstype="FSLIST"] "
		"--fsname=<filesystem name>\n"
		"\t--index=<target index> [options] <device>\n", progname);
#ifdef HAVE_ZFS_OSD
	fprintf(out, "usage: %s <target type> --backfstype=zfs "
		"--fsname=<filesystem name> [options]\n"
		"\t<pool name>/<dataset name>\n"
		"\t[[<vdev type>] <device> [<device> ...] [vdev type>] ...]\n",
		progname);
#endif
	fprintf(out,
		"\t<device>:block device or file (e.g /dev/sda or /tmp/ost1)\n"
#ifdef HAVE_ZFS_OSD
		"\t<pool name>: name of ZFS pool where target is created "
			"(e.g. tank)\n"
		"\t<dataset name>: name of new dataset, must be unique within "
			"pool (e.g. ost1)\n"
		"\t<vdev type>: type of vdev (mirror, raidz, raidz2, spare, "
			"cache, log)\n"
#endif
		"\n"
#ifndef TUNEFS
		"\ttarget types:\n"
		"\t\t--mgs: configuration management service\n"
		"\t\t--mdt: metadata storage, mutually exclusive with ost\n"
		"\t\t--ost: object storage, mutually exclusive with mdt, mgs\n"
#endif
		"\toptions (in order of popularity):\n"
		"\t\t--index=#N: numerical target index (0..N)\n"
		"\t\t\trequired for all targets other than the MGS\n"
		"\t\t--fsname=<8_char_filesystem_name>: fs targets belong to\n"
		"\t\t\trequired for all targets other than MGS\n"
		"\t\t--mgsnode=<nid>[,<...>]: NID(s) of remote MGS\n"
		"\t\t\trequired for all targets other than MGS\n"
		"\t\t--mountfsoptions=<opts>: permanent mount options\n"
		"\t\t--failnode=<nid>[,<...>]: NID(s) of backup failover node\n"
		"\t\t\tmutually exclusive with --servicenode\n"
		"\t\t--servicenode=<nid>[,<...>]: NID(s) of service partners\n"
		"\t\t\ttreat nodes as equal service node, mutually exclusive "
			"with --failnode\n"
		"\t\t--param <key>=<value>: set a permanent parameter\n"
		"\t\t\te.g. --param sys.timeout=40\n"
		"\t\t\t     --param lov.stripesize=2M\n"
		"\t\t--network=<net>[,<...>]: restrict OST/MDT to network(s)\n"
#ifndef TUNEFS
		"\t\t--backfstype=<fstype>: backing fs type (ldiskfs, zfs)\n"
		"\t\t--device-size=#N(KB): device size for loop devices\n"
		"\t\t--mkfsoptions=<opts>: format options\n"
		"\t\t--reformat: overwrite an existing disk\n"
		"\t\t--replace: replace an old target with the same index\n"
		"\t\t--stripe-count-hint=#N: for optimizing MDT inode size\n"
#else
		"\t\t--erase-param <key>: erase all instances of a parameter\n"
		"\t\t--erase-params: erase all old parameter settings\n"
		"\t\t--nomgs: turn off MGS service on this MDT\n"
		"\t\t--writeconf: erase all config logs for this fs.\n"
		"\t\t--quota: enable space accounting on old 2.x device.\n"
#endif
		"\t\t--comment=<user comment>: arbitrary string (%d bytes)\n"
		"\t\t--dryrun: report what we would do; don't write to disk\n"
		"\t\t--verbose: e.g. show mkfs progress\n"
		"\t\t-V|--version: output build version of the utility and\n"
		"\t\t\texit\n"
		"\t\t--quiet\n",
		(int)sizeof(((struct lustre_disk_data *)0)->ldd_userdata));
	return;
}

/* ==================== Lustre config functions =============*/

void print_ldd(char *str, struct lustre_disk_data *ldd)
{
        printf("\n   %s:\n", str);
        printf("Target:     %s\n", ldd->ldd_svname);
        if (ldd->ldd_svindex == INDEX_UNASSIGNED)
                printf("Index:      unassigned\n");
        else
                printf("Index:      %d\n", ldd->ldd_svindex);
        if (ldd->ldd_uuid[0])
                printf("UUID:       %s\n", (char *)ldd->ldd_uuid);
        printf("Lustre FS:  %s\n", ldd->ldd_fsname);
        printf("Mount type: %s\n", MT_STR(ldd));
        printf("Flags:      %#x\n", ldd->ldd_flags);
	printf("              (%s%s%s%s%s%s%s%s%s)\n",
               IS_MDT(ldd) ? "MDT ":"",
               IS_OST(ldd) ? "OST ":"",
               IS_MGS(ldd) ? "MGS ":"",
               ldd->ldd_flags & LDD_F_NEED_INDEX ? "needs_index ":"",
               ldd->ldd_flags & LDD_F_VIRGIN     ? "first_time ":"",
               ldd->ldd_flags & LDD_F_UPDATE     ? "update ":"",
               ldd->ldd_flags & LDD_F_WRITECONF  ? "writeconf ":"",
               ldd->ldd_flags & LDD_F_NO_PRIMNODE? "no_primnode ":"",
               ldd->ldd_flags & LDD_F_UPGRADE14  ? "upgrade1.4 ":"");
        printf("Persistent mount opts: %s\n", ldd->ldd_mount_opts);
        printf("Parameters:%s\n", ldd->ldd_params);
        if (ldd->ldd_userdata[0])
                printf("Comment: %s\n", ldd->ldd_userdata);
        printf("\n");
}

void set_defaults(struct mkfs_opts *mop)
{
	mop->mo_ldd.ldd_magic = LDD_MAGIC;
	mop->mo_ldd.ldd_config_ver = 1;
	mop->mo_ldd.ldd_flags = LDD_F_NEED_INDEX | LDD_F_UPDATE | LDD_F_VIRGIN;
#ifdef HAVE_LDISKFS_OSD
	mop->mo_ldd.ldd_mount_type = LDD_MT_LDISKFS;
#else
	mop->mo_ldd.ldd_mount_type = LDD_MT_ZFS;
#endif
	mop->mo_ldd.ldd_svindex = INDEX_UNASSIGNED;
	mop->mo_mgs_failnodes = 0;
	mop->mo_stripe_count = 1;
	mop->mo_pool_vdevs = NULL;
}

static inline void badopt(const char *opt, char *type)
{
        fprintf(stderr, "%s: '--%s' only valid for %s\n",
                progname, opt, type);
        usage(stderr);
}

#ifdef TUNEFS
/**
 * Removes all existing instances of the parameter passed in \a param, that are
 * in the form of "key=<value>", from the character buffer at \a buf.
 *
 * The parameter in \a param can be either in the form of "key" when the call is
 * made from \a parse_opts() while handling an "--erase-param" option, or in the
 * form of "key=<value>" when the call is made from \a parse_opts() while
 * handling a "--param" option.
 *
 * \param buf the buffer holding on-disk server parameters.
 * \param param the parameter whose instances are to be removed from \a buf.
 * \param withval when true \a param includes a value, e.g. when handling a
 *		  "--param p=<val>" option; when false \a param only includes
 *		  the parameter key, without a value, e.g. when handling an
 *		  "--erase-param p" option.
 *
 * \retval 0 success; parameter was erased, or not found.
 * \retval EINVAL failure, invalid input parameter.
 */
static int erase_param(const char *const buf, const char *const param,
		       bool withval)
{
	char	search[PARAM_MAX + 1];

	if (strlen(param) > PARAM_MAX) {
		fprintf(stderr, "%s: param to erase is too long-\n%s\n",
			progname, param);
		return EINVAL;
	}

	/**
	 * \a add_param() writes a space as the first character in \a ldd_params
	 */
	search[0] = ' ';

	/** Populate the rest of the 'search' array depending on what type of
	 * option is being handled in parse_opts().
	 */
	if (withval) {
		char *keyend;

		keyend = strchr(param, '=');
		if (keyend == NULL)
			return EINVAL;
		strncpy(&search[1], param, keyend - param + 1);
		search[keyend - param + 2] = '\0';
	} else {
		strcpy(&search[1], param);
		strcat(search, "=");
	}

	while (1) {
		char	*found;
		char	*space;

		found = strstr(buf, search);
		if (found == NULL)
			return 0;
		space = strchr(found + 1, ' ');
		if (space != NULL) {
			memmove(found, space, strlen(space) + 1);
		} else {
			/* Reached the end of the string at buf; parameter at
			 * found is the last one.
			 */
			*found = '\0';
			return 0;
		}
	}
}

/**
 * Checks whether the parameter at \a param, given in the form of "key=<value>"
 * is the first instance of this parameter type specified in the command line.
 *
 * \param argv array of program argument strings from \a main().
 * \param argvidx index in \a argv of the parameter currently being handled in
 * \a parse_opts().
 * \param param the parameter to be searched, in the form of
 * \a "key=<value>".
 *
 * \retval 0 parameter instance is not the first parameter instance of this
 * type specified in the command line.
 * \retval 1 parameter instance is the first parameter instance of this
 * type specified in the command line.
 * \retval EINVAL invalid input parameter.
 *
 * N.B.: this function will not work if the short option '-p' is used for
 *	 --param
 */
static int param_is_first(char *const argv[], const int argvidx,
			  const char *const param)
{
	int	i;
	char   *keyend;
	size_t	keylen;
	size_t	longkeylen;
	char	key[PARAM_MAX - 1];
	char	longkey[PARAM_MAX + 9];

	if (strlen(param) > PARAM_MAX) {
		fprintf(stderr, "%s: param to erase is too long-\n%s\n",
			progname, param);
		return EINVAL;
	}

	keyend = strchr(param, '=');

	/* All parameters handled by tunefs.lustre are of the form "key=value".
	 */
	if (keyend == NULL)
		return EINVAL;

	/* Get the parameter key. */
	strncpy(key, param, keyend - param + 1);
	key[keyend - param + 1] = '\0';
	keylen = strlen(key);

	/* Command line options can be given either in the form
	 * "--param key=<value>", or in the form "--param=key=value"; in the
	 * latter case, the string in argv will also contain the "--param="
	 * substring; produce a longkey to cover the latter case.
	 */
	strcpy(longkey, "--param=");
	strcat(longkey, key);
	longkeylen = strlen(longkey);

	for (i = argvidx - 1; i > 0; i--)
		if (strncmp(argv[i], key, keylen) == 0 ||
		    strncmp(argv[i], longkey, longkeylen) == 0)
			return 0;
	return 1;
}
#endif

/* from mount_lustre */
/* Get rid of symbolic hostnames for tcp, since kernel can't do lookups */
#define MAXNIDSTR 1024
static char *convert_hostnames(char *s1)
{
        char *converted, *s2 = 0, *c, *end, sep;
        int left = MAXNIDSTR;
        lnet_nid_t nid;

        converted = malloc(left);
        if (converted == NULL) {
                return NULL;
        }

        end = s1 + strlen(s1);
        c = converted;
        while ((left > 0) && (s1 < end)) {
                s2 = strpbrk(s1, ",:");
                if (!s2)
                        s2 = end;
                sep = *s2;
                *s2 = '\0';
                nid = libcfs_str2nid(s1);
		*s2 = sep;

                if (nid == LNET_NID_ANY) {
			fprintf(stderr, "%s: Cannot resolve hostname '%s'.\n",
				progname, s1);
                        free(converted);
                        return NULL;
                }
                if (strncmp(libcfs_nid2str(nid), "127.0.0.1",
                            strlen("127.0.0.1")) == 0) {
                        fprintf(stderr, "%s: The NID '%s' resolves to the "
                                "loopback address '%s'.  Lustre requires a "
                                "non-loopback address.\n",
                                progname, s1, libcfs_nid2str(nid));
                        free(converted);
                        return NULL;
                }

                c += snprintf(c, left, "%s%c", libcfs_nid2str(nid), sep);
                left = converted + MAXNIDSTR - c;
                s1 = s2 + 1;
        }
        return converted;
}

int parse_opts(int argc, char *const argv[], struct mkfs_opts *mop,
	       char **mountopts)
{
	static struct option long_opt[] = {
		{ "failnode",		required_argument,	NULL, 'f' },
		{ "failover",		required_argument,	NULL, 'f' },
		{ "help",		no_argument,		NULL, 'h' },
		{ "index",		required_argument,	NULL, 'i' },
		{ "fsname",		required_argument,	NULL, 'L' },
		{ "mgsnode",		required_argument,	NULL, 'm' },
		{ "mgsnid",		required_argument,	NULL, 'm' },
		{ "dryrun",		no_argument,		NULL, 'n' },
		{ "mountfsoptions",	required_argument,	NULL, 'o' },
		{ "param",		required_argument,	NULL, 'p' },
		{ "quiet",		no_argument,		NULL, 'q' },
		{ "servicenode",	required_argument,	NULL, 's' },
		{ "network",		required_argument,	NULL, 't' },
		{ "comment",		required_argument,	NULL, 'u' },
		{ "verbose",		no_argument,		NULL, 'v' },
		{ "version",		no_argument,		NULL, 'V' },
#ifndef TUNEFS
		{ "backfstype",		required_argument,	NULL, 'b' },
		{ "stripe-count-hint",	required_argument,	NULL, 'c' },
		{ "device-size",	required_argument,	NULL, 'd' },
		{ "mgs",		no_argument,		NULL, 'G' },
		{ "mkfsoptions",	required_argument,	NULL, 'k' },
		{ "mdt",		no_argument,		NULL, 'M' },
		{ "nomgs",		no_argument,		NULL, 'N' },
		{ "ost",		no_argument,		NULL, 'O' },
		{ "reformat",		no_argument,		NULL, 'r' },
		{ "replace",		no_argument,		NULL, 'R' },
#else
		{ "erase-param",	required_argument,	NULL, 'E' },
		{ "erase-params",	no_argument,		NULL, 'e' },
		{ "quota",		no_argument,		NULL, 'Q' },
		{ "writeconf",		no_argument,		NULL, 'w' },
#endif
		{ 0,			0,			NULL,  0  }
	};
	char *optstring = "f:hi:L:m:no:p:qs:t:u:vV"
#ifndef TUNEFS
			  "b:c:d:Gk:MNOrR";
#else
			  "E:eQw";
#endif
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	char new_fsname[16] = "";
	int opt;
	int rc, longidx;
	int failnode_set = 0, servicenode_set = 0;
	int replace = 0;

	while ((opt = getopt_long(argc, argv, optstring, long_opt, &longidx)) !=
	       EOF) {
		switch (opt) {
		case 'f':
		case 's': {
			char *nids;

			if ((opt == 'f' && servicenode_set) ||
			    (opt == 's' && failnode_set)) {
				fprintf(stderr, "%s: %s cannot use with --%s\n",
					progname, long_opt[longidx].name,
					opt == 'f' ? "servicenode" : "failnode");
				return 1;
			}

			nids = convert_hostnames(optarg);
			if (nids == NULL)
				return 1;

			rc = append_param(ldd->ldd_params, PARAM_FAILNODE,
					  nids, ':');
			free(nids);
			if (rc != 0)
				return rc;

			/* Must update the mgs logs */
			ldd->ldd_flags |= LDD_F_UPDATE;
			if (opt == 'f') {
				failnode_set = 1;
			} else {
				ldd->ldd_flags |= LDD_F_NO_PRIMNODE;
				servicenode_set = 1;
			}
			mop->mo_flags |= MO_FAILOVER;
			break;
		}
		case 'h':
			usage(stdout);
			return 0;
		case 'i':
			if (!(ldd->ldd_flags &
			      (LDD_F_UPGRADE14 | LDD_F_VIRGIN |
			       LDD_F_WRITECONF))) {
				fprintf(stderr, "%s: cannot change the index "
					"of a registered target\n", progname);
				return 1;
			}

			/* LU-2374: check whether it is OST/MDT later */
			ldd->ldd_svindex = atol(optarg);
			if (ldd->ldd_svindex >= INDEX_UNASSIGNED) {
				fprintf(stderr, "%s: wrong index %u. "
					"Target index must be less than %u.\n",
					progname, ldd->ldd_svindex,
					INDEX_UNASSIGNED);
				return 1;
			}

			ldd->ldd_flags &= ~LDD_F_NEED_INDEX;
			break;
		case 'L': {
			char *tmp;

			if ((strlen(optarg) < 1) || (strlen(optarg) > 8)) {
				fprintf(stderr, "%s: filesystem name must be "
					"1-8 chars\n", progname);
				return 1;
			}

			if ((tmp = strpbrk(optarg, "/:"))) {
				fprintf(stderr, "%s: char '%c' not allowed in "
					"filesystem name\n", progname, *tmp);
				return 1;
			}

			strscpy(new_fsname, optarg, sizeof(new_fsname));
			break;
		}
		case 'm': {
			char *nids = convert_hostnames(optarg);

			if (nids == NULL)
				return 1;

			rc = append_param(ldd->ldd_params, PARAM_MGSNODE,
					  nids, ':');
			free(nids);
			if (rc != 0)
				return rc;

			mop->mo_mgs_failnodes++;
			break;
		}
		case 'n':
			print_only++;
			break;
		case 'o':
			*mountopts = optarg;
			break;
		case 'p':
#ifdef TUNEFS
			/* Erase all stored instances of the parameter, when the
			 * first parameter instance in the command line is
			 * handled.
			 */
			rc = param_is_first(argv, optind - 1, optarg);
			if (rc == EINVAL)
				return rc;
			if (rc == 1) {
				rc = erase_param(ldd->ldd_params, optarg,
						 true);
				if (rc != 0)
					return rc;
				/* Update the mgs logs, in case add_param()
				 * below fails, but erase_param() altered
				 * the disk data */
				ldd->ldd_flags |= LDD_F_UPDATE;
			}
#endif
			rc = add_param(ldd->ldd_params, NULL, optarg);
			if (rc != 0)
				return rc;

			/* Must update the mgs logs */
			ldd->ldd_flags |= LDD_F_UPDATE;
			break;
		case 'q':
			verbose--;
			break;
		case 't':
			if (!IS_MDT(ldd) && !IS_OST(ldd)) {
				badopt(long_opt[longidx].name, "MDT,OST");
				return 1;
			}

			if (optarg == NULL)
				return 1;

			rc = add_param(ldd->ldd_params, PARAM_NETWORK, optarg);
			if (rc != 0)
				return rc;

			/* Must update the mgs logs */
			ldd->ldd_flags |= LDD_F_UPDATE;
			break;
		case 'u':
			strscpy(ldd->ldd_userdata, optarg,
				sizeof(ldd->ldd_userdata));
			break;
		case 'v':
			verbose++;
			break;
		case 'V':
			++version;
			fprintf(stdout, "%s %s\n", progname,
				LUSTRE_VERSION_STRING);
			return 0;
#ifndef TUNEFS
		case 'b': {
			int i = 0;

			do {
				if (strcmp(optarg, mt_str(i)) == 0) {
					ldd->ldd_mount_type = i;
					break;
				}
			} while (++i < LDD_MT_LAST);

			if (i == LDD_MT_LAST) {
				fprintf(stderr, "%s: invalid backend filesystem"
					" type %s\n", progname, optarg);
				return 1;
			}
			break;
		}
		case 'c':
			if (IS_MDT(ldd)) {
				int stripe_count = atol(optarg);

				if (stripe_count <= 0) {
					fprintf(stderr, "%s: bad stripe count "
						"%d\n", progname, stripe_count);
					return 1;
				}
				mop->mo_stripe_count = stripe_count;
			} else {
				badopt(long_opt[longidx].name, "MDT");
				return 1;
			}
			break;
		case 'd':
			mop->mo_device_kb = atol(optarg);
			break;
		case 'G':
			ldd->ldd_flags |= LDD_F_SV_TYPE_MGS;
			break;
		case 'k':
			strscpy(mop->mo_mkfsopts, optarg,
				sizeof(mop->mo_mkfsopts));
			break;
		case 'M':
			ldd->ldd_flags |= LDD_F_SV_TYPE_MDT;
			break;
		case 'N':
			ldd->ldd_flags &= ~LDD_F_SV_TYPE_MGS;
			break;
		case 'O':
			ldd->ldd_flags |= LDD_F_SV_TYPE_OST;
			break;
		case 'r':
			mop->mo_flags |= MO_FORCEFORMAT;
			break;
		case 'R':
			replace = 1;
			break;
#else /* !TUNEFS */
		case 'E':
			rc = erase_param(ldd->ldd_params, optarg, false);
			if (rc != 0)
				return rc;
			/* Must update the mgs logs */
			ldd->ldd_flags |= LDD_F_UPDATE;
			break;
		case 'e':
			ldd->ldd_params[0] = '\0';
			/* Must update the mgs logs */
			ldd->ldd_flags |= LDD_F_UPDATE;
			break;
		case 'Q':
			mop->mo_flags |= MO_QUOTA;
			break;
		case 'w':
			ldd->ldd_flags |= LDD_F_WRITECONF;
			break;
#endif /* !TUNEFS */
		default:
			if (opt != '?') {
				fatal();
				fprintf(stderr, "Unknown option '%c'\n", opt);
			}
			return EINVAL;
		}
	}//while

	if (new_fsname[0] != '\0') {
		if (!(mop->mo_flags & MO_FORCEFORMAT) &&
		     (!(ldd->ldd_flags &
			(LDD_F_UPGRADE14 | LDD_F_VIRGIN | LDD_F_WRITECONF)))) {
			fprintf(stderr, "%s: cannot change the name "
				"of a registered target\n", progname);
			return 1;
		}

		strscpy(ldd->ldd_fsname, new_fsname, sizeof(ldd->ldd_fsname));
	}

	/* Need to clear this flag after parsing 'L' and 'i' options. */
	if (replace)
		ldd->ldd_flags &= ~LDD_F_VIRGIN;

	if (optind == argc) {
		/* The user didn't specify device name */
		fatal();
		fprintf(stderr, "Not enough arguments - device name or "
			"pool/dataset name not specified.\n");
		return EINVAL;
	} else {
		/*  The device or pool/filesystem name */
		strscpy(mop->mo_device, argv[optind], sizeof(mop->mo_device));

		/* Followed by optional vdevs */
		if (optind < argc - 1)
			mop->mo_pool_vdevs = (char **) &argv[optind + 1];
	}

	return 0;
}

int main(int argc, char *const argv[])
{
	struct mkfs_opts mop;
	struct lustre_disk_data *ldd = &mop.mo_ldd;
	char *mountopts = NULL;
	char always_mountopts[512] = "";
	char default_mountopts[512] = "";
	unsigned mount_type;
	int ret = 0;
	int ret2 = 0;

	if ((progname = strrchr(argv[0], '/')) != NULL)
		progname++;
	else
		progname = argv[0];

	if ((argc < 2) || (argv[argc - 1][0] == '-')) {
		usage(stderr);
		return(EINVAL);
	}

	memset(&mop, 0, sizeof(mop));
	set_defaults(&mop);

	/* device is last arg */
	strscpy(mop.mo_device, argv[argc - 1], sizeof(mop.mo_device));

	ret = osd_init();
	if (ret != 0)
		return ret;

#ifdef TUNEFS
	/* For tunefs, we must read in the old values before parsing any
	   new ones. */

	/* Check whether the disk has already been formatted by mkfs.lustre */
	ret = osd_is_lustre(mop.mo_device, &mount_type);
	if (ret == 0) {
		fatal();
		fprintf(stderr, "Device %s has not been formatted with "
			"mkfs.lustre\n", mop.mo_device);
		ret = ENODEV;
		goto out;
	}
	ldd->ldd_mount_type = mount_type;

	ret = osd_read_ldd(mop.mo_device, ldd);
	if (ret != 0) {
		fatal();
		fprintf(stderr, "Failed to read previous Lustre data from %s "
			"(%d)\n", mop.mo_device, ret);
		goto out;
	}

	ldd->ldd_flags &= ~(LDD_F_WRITECONF | LDD_F_VIRGIN);

	/* svname of the form lustre:OST1234 means never registered */
	ret = strlen(ldd->ldd_svname);
	if (ldd->ldd_svname[ret - 8] == ':') {
		ldd->ldd_svname[ret - 8] = '-';
		ldd->ldd_flags |= LDD_F_VIRGIN;
	} else if (ldd->ldd_svname[ret - 8] == '=') {
		ldd->ldd_svname[ret - 8] = '-';
		ldd->ldd_flags |= LDD_F_WRITECONF;
	}

	if (strstr(ldd->ldd_params, PARAM_MGSNODE))
		mop.mo_mgs_failnodes++;

	if (verbose > 0)
		print_ldd("Read previous values", ldd);
#endif /* TUNEFS */

	ret = parse_opts(argc, argv, &mop, &mountopts);
	if (ret != 0 || version)
		goto out;

	if (!(IS_MDT(ldd) || IS_OST(ldd) || IS_MGS(ldd))) {
		fatal();
		fprintf(stderr, "must set target type: MDT,OST,MGS\n");
		ret = EINVAL;
		goto out;
	}

	if (((IS_MDT(ldd) || IS_MGS(ldd))) && IS_OST(ldd)) {
		fatal();
		fprintf(stderr, "OST type is exclusive with MDT,MGS\n");
		ret = EINVAL;
		goto out;
	}

	/* Stand alone MGS doesn't need a index */
	if (!IS_MDT(ldd) && IS_MGS(ldd)) {
#ifndef TUNEFS /* mkfs.lustre */
		/* But if --index was specified flag an error */
		if (!(ldd->ldd_flags & LDD_F_NEED_INDEX)) {
			badopt("index", "MDT,OST");
			goto out;
		}
#endif
		ldd->ldd_flags &= ~LDD_F_NEED_INDEX;
	}

	if ((ldd->ldd_flags & (LDD_F_NEED_INDEX | LDD_F_UPGRADE14)) ==
	    (LDD_F_NEED_INDEX | LDD_F_UPGRADE14)) {
		fatal();
		fprintf(stderr, "Can't find the target index, "
		"specify with --index\n");
		ret = EINVAL;
		goto out;
	}

	if (ldd->ldd_flags & LDD_F_NEED_INDEX)
		fprintf(stderr, "warning: %s: for Lustre 2.4 and later, the "
			"target index must be specified with --index\n",
			mop.mo_device);

	/* If no index is supplied for MDT by default set index to zero */
	if (IS_MDT(ldd) && (ldd->ldd_svindex == INDEX_UNASSIGNED)) {
		ldd->ldd_flags &= ~LDD_F_NEED_INDEX;
		ldd->ldd_svindex = 0;
	}
	if (!IS_MGS(ldd) && (mop.mo_mgs_failnodes == 0)) {
		fatal();
		if (IS_MDT(ldd))
			fprintf(stderr, "Must specify --mgs or --mgsnode\n");
		else
			fprintf(stderr, "Must specify --mgsnode\n");
		ret = EINVAL;
		goto out;
	}
	if ((IS_MDT(ldd) || IS_OST(ldd)) && ldd->ldd_fsname[0] == '\0') {
		fatal();
		fprintf(stderr, "Must specify --fsname for MDT/OST device\n");
		ret = EINVAL;
		goto out;
	}

	/* These are the permanent mount options (always included) */
	ret = osd_prepare_lustre(&mop,
				 default_mountopts, sizeof(default_mountopts),
				 always_mountopts, sizeof(always_mountopts));
	if (ret != 0) {
		fatal();
		fprintf(stderr, "unable to prepare backend (%d)\n", ret);
		goto out;
	}

	if (mountopts) {
		trim_mountfsoptions(mountopts);
		(void)check_mountfsoptions(mountopts, default_mountopts, 1);
		if (check_mountfsoptions(mountopts, always_mountopts, 0)) {
			ret = EINVAL;
			goto out;
		}
		sprintf(ldd->ldd_mount_opts, "%s", mountopts);
	} else {
#ifdef TUNEFS
		if (ldd->ldd_mount_opts[0] == 0)
			/* use the defaults unless old opts exist */
#endif
		{
			sprintf(ldd->ldd_mount_opts, "%s%s",
				always_mountopts, default_mountopts);
			trim_mountfsoptions(ldd->ldd_mount_opts);
		}
	}

	server_make_name(ldd->ldd_flags, ldd->ldd_svindex,
			ldd->ldd_fsname, ldd->ldd_svname);

	if (verbose >= 0)
		print_ldd("Permanent disk data", ldd);

	if (print_only) {
		printf("exiting before disk write.\n");
		goto out;
	}

	if (check_mtab_entry(mop.mo_device, mop.mo_device, NULL, NULL))
		return(EEXIST);

	/* Create the loopback file */
	if (mop.mo_flags & MO_IS_LOOP) {
		ret = access(mop.mo_device, F_OK);
		if (ret != 0)
			ret = errno;

#ifndef TUNEFS /* mkfs.lustre */
		/* Reformat the loopback file */
		if (ret != 0 || (mop.mo_flags & MO_FORCEFORMAT)) {
			ret = loop_format(&mop);
			if (ret != 0)
				goto out;
		}
#endif
		if (ret == 0)
			ret = loop_setup(&mop);
		if (ret != 0) {
			fatal();
			fprintf(stderr, "Loop device setup for %s failed: %s\n",
					mop.mo_device, strerror(ret));
			goto out;
		}
	}

#ifndef TUNEFS /* mkfs.lustre */
	/* Check whether the disk has already been formatted by mkfs.lustre */
	if (!(mop.mo_flags & MO_FORCEFORMAT)) {
		ret = osd_is_lustre(mop.mo_device, &mount_type);
		if (ret != 0) {
			fatal();
			fprintf(stderr, "Device %s was previously formatted "
				"for lustre. Use --reformat to reformat it, "
				"or tunefs.lustre to modify.\n",
				mop.mo_device);
			goto out;
		}
	}

	/* Format the backing filesystem */
	ret = osd_make_lustre(&mop);
	if (ret != 0) {
		fatal();
		fprintf(stderr, "mkfs failed %d\n", ret);
		goto out;
	}
#else /* !TUNEFS */
	/* update svname with '=' to refresh config */
	if (ldd->ldd_flags & LDD_F_WRITECONF) {
		struct mount_opts opts;
		opts.mo_ldd = *ldd;
		opts.mo_source = mop.mo_device;
		(void) osd_label_lustre(&opts);
	}

	/* Enable quota accounting */
	if (mop.mo_flags & MO_QUOTA) {
		ret = osd_enable_quota(&mop);
		goto out;
	}
#endif /* !TUNEFS */

	/* Write our config files */
	ret = osd_write_ldd(&mop);
	if (ret != 0) {
		fatal();
		fprintf(stderr, "failed to write local files\n");
		goto out;
	}

out:
	osd_fini();
	ret2 = loop_cleanup(&mop);
	if (ret == 0)
		ret = ret2;

	/* Fix any crazy return values from system() */
	if (ret != 0 && ((ret & 255) == 0))
		return (1);

	if (ret != 0)
		verrprint("%s: exiting with %d (%s)\n",
			  progname, ret, strerror(ret));
	return (ret);
}
