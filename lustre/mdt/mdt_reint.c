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
 * Copyright (c) 2011 Whamcloud, Inc.
 *
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/mdt/mdt_reint.c
 *
 * Lustre Metadata Target (mdt) reintegration routines
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Andreas Dilger <adilger@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Huang Hua <huanghua@clusterfs.com>
 * Author: Yury Umanets <umka@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_MDS

#include "mdt_internal.h"
#include "lu_time.h"

static inline void mdt_reint_init_ma(struct mdt_thread_info *info,
                                     struct md_attr *ma)
{
        ma->ma_lmm = req_capsule_server_get(info->mti_pill, &RMF_MDT_MD);
        ma->ma_lmm_size = req_capsule_get_size(info->mti_pill,
                                               &RMF_MDT_MD, RCL_SERVER);

        ma->ma_cookie = req_capsule_server_get(info->mti_pill,
                                               &RMF_LOGCOOKIES);
        ma->ma_cookie_size = req_capsule_get_size(info->mti_pill,
                                                  &RMF_LOGCOOKIES,
                                                  RCL_SERVER);

        ma->ma_need = MA_INODE | MA_LOV | MA_COOKIE;
        ma->ma_valid = 0;
}

static int mdt_create_pack_capa(struct mdt_thread_info *info, int rc,
                                struct mdt_object *object,
                                struct mdt_body *repbody)
{
        ENTRY;

        /* for cross-ref mkdir, mds capa has been fetched from remote obj, then
         * we won't go to below*/
        if (repbody->valid & OBD_MD_FLMDSCAPA)
                RETURN(rc);

        if (rc == 0 && info->mti_mdt->mdt_opts.mo_mds_capa &&
            info->mti_exp->exp_connect_data.ocd_connect_flags & OBD_CONNECT_MDS_CAPA) {
                struct lustre_capa *capa;

                capa = req_capsule_server_get(info->mti_pill, &RMF_CAPA1);
                LASSERT(capa);
                capa->lc_opc = CAPA_OPC_MDS_DEFAULT;
                rc = mo_capa_get(info->mti_env, mdt_object_child(object), capa,
                                 0);
                if (rc == 0)
                        repbody->valid |= OBD_MD_FLMDSCAPA;
        }

        RETURN(rc);
}

/**
 * Get version of object by fid.
 *
 * Return real version or ENOENT_VERSION if object doesn't exist
 */
static void mdt_obj_version_get(struct mdt_thread_info *info,
                                struct mdt_object *o, __u64 *version)
{
        LASSERT(o);
        LASSERT(mdt_object_exists(o) >= 0);
        if (mdt_object_exists(o) > 0 && !mdt_object_obf(o))
                *version = mo_version_get(info->mti_env, mdt_object_child(o));
        else
                *version = ENOENT_VERSION;
        CDEBUG(D_INODE, "FID "DFID" version is "LPX64"\n",
               PFID(mdt_object_fid(o)), *version);
}

/**
 * Check version is correct.
 *
 * Should be called only during replay.
 */
static int mdt_version_check(struct ptlrpc_request *req,
                             __u64 version, int idx)
{
        __u64 *pre_ver = lustre_msg_get_versions(req->rq_reqmsg);
        ENTRY;

        if (!exp_connect_vbr(req->rq_export))
                RETURN(0);

        LASSERT(req_is_replay(req));
        /** VBR: version is checked always because costs nothing */
        LASSERT(idx < PTLRPC_NUM_VERSIONS);
        /** Sanity check for malformed buffers */
        if (pre_ver == NULL) {
                CERROR("No versions in request buffer\n");
                cfs_spin_lock(&req->rq_export->exp_lock);
                req->rq_export->exp_vbr_failed = 1;
                cfs_spin_unlock(&req->rq_export->exp_lock);
                RETURN(-EOVERFLOW);
        } else if (pre_ver[idx] != version) {
                CDEBUG(D_INODE, "Version mismatch "LPX64" != "LPX64"\n",
                       pre_ver[idx], version);
                cfs_spin_lock(&req->rq_export->exp_lock);
                req->rq_export->exp_vbr_failed = 1;
                cfs_spin_unlock(&req->rq_export->exp_lock);
                RETURN(-EOVERFLOW);
        }
        RETURN(0);
}

/**
 * Save pre-versions in reply.
 */
static void mdt_version_save(struct ptlrpc_request *req, __u64 version,
                             int idx)
{
        __u64 *reply_ver;

        if (!exp_connect_vbr(req->rq_export))
                return;

        LASSERT(!req_is_replay(req));
        LASSERT(req->rq_repmsg != NULL);
        reply_ver = lustre_msg_get_versions(req->rq_repmsg);
        if (reply_ver)
                reply_ver[idx] = version;
}

/**
 * Save enoent version, it is needed when it is obvious that object doesn't
 * exist, e.g. child during create.
 */
static void mdt_enoent_version_save(struct mdt_thread_info *info, int idx)
{
        /* save version of file name for replay, it must be ENOENT here */
        if (!req_is_replay(mdt_info_req(info))) {
                info->mti_ver[idx] = ENOENT_VERSION;
                mdt_version_save(mdt_info_req(info), info->mti_ver[idx], idx);
        }
}

/**
 * Get version from disk and save in reply buffer.
 *
 * Versions are saved in reply only during normal operations not replays.
 */
void mdt_version_get_save(struct mdt_thread_info *info,
                          struct mdt_object *mto, int idx)
{
        /* don't save versions during replay */
        if (!req_is_replay(mdt_info_req(info))) {
                mdt_obj_version_get(info, mto, &info->mti_ver[idx]);
                mdt_version_save(mdt_info_req(info), info->mti_ver[idx], idx);
        }
}

/**
 * Get version from disk and check it, no save in reply.
 */
int mdt_version_get_check(struct mdt_thread_info *info,
                          struct mdt_object *mto, int idx)
{
        /* only check versions during replay */
        if (!req_is_replay(mdt_info_req(info)))
                return 0;

        mdt_obj_version_get(info, mto, &info->mti_ver[idx]);
        return mdt_version_check(mdt_info_req(info), info->mti_ver[idx], idx);
}

/**
 * Get version from disk and check if recovery or just save.
 */
int mdt_version_get_check_save(struct mdt_thread_info *info,
                               struct mdt_object *mto, int idx)
{
        int rc = 0;

        mdt_obj_version_get(info, mto, &info->mti_ver[idx]);
        if (req_is_replay(mdt_info_req(info)))
                rc = mdt_version_check(mdt_info_req(info), info->mti_ver[idx],
                                       idx);
        else
                mdt_version_save(mdt_info_req(info), info->mti_ver[idx], idx);
        return rc;
}

/**
 * Lookup with version checking.
 *
 * This checks version of 'name'. Many reint functions uses 'name' for child not
 * FID, therefore we need to get object by name and check its version.
 */
int mdt_lookup_version_check(struct mdt_thread_info *info,
                             struct mdt_object *p, struct lu_name *lname,
                             struct lu_fid *fid, int idx)
{
        int rc, vbrc;

        rc = mdo_lookup(info->mti_env, mdt_object_child(p), lname, fid,
                        &info->mti_spec);
        /* Check version only during replay */
        if (!req_is_replay(mdt_info_req(info)))
                return rc;

        info->mti_ver[idx] = ENOENT_VERSION;
        if (rc == 0) {
                struct mdt_object *child;
                child = mdt_object_find(info->mti_env, info->mti_mdt, fid);
                if (likely(!IS_ERR(child))) {
                        mdt_obj_version_get(info, child, &info->mti_ver[idx]);
                        mdt_object_put(info->mti_env, child);
                }
        }
        vbrc = mdt_version_check(mdt_info_req(info), info->mti_ver[idx], idx);
        return vbrc ? vbrc : rc;

}

/*
 * VBR: we save three versions in reply:
 * 0 - parent. Check that parent version is the same during replay.
 * 1 - name. Version of 'name' if file exists with the same name or
 * ENOENT_VERSION, it is needed because file may appear due to missed replays.
 * 2 - child. Version of child by FID. Must be ENOENT. It is mostly sanity
 * check.
 */
static int mdt_md_create(struct mdt_thread_info *info)
{
        struct mdt_device       *mdt = info->mti_mdt;
        struct mdt_object       *parent;
        struct mdt_object       *child;
        struct mdt_lock_handle  *lh;
        struct mdt_body         *repbody;
        struct md_attr          *ma = &info->mti_attr;
        struct mdt_reint_record *rr = &info->mti_rr;
        struct lu_name          *lname;
        int rc;
        ENTRY;

        DEBUG_REQ(D_INODE, mdt_info_req(info), "Create  (%s->"DFID") in "DFID,
                  rr->rr_name, PFID(rr->rr_fid2), PFID(rr->rr_fid1));

        repbody = req_capsule_server_get(info->mti_pill, &RMF_MDT_BODY);

        lh = &info->mti_lh[MDT_LH_PARENT];
        mdt_lock_pdo_init(lh, LCK_PW, rr->rr_name, rr->rr_namelen);

        parent = mdt_object_find_lock(info, rr->rr_fid1, lh,
                                      MDS_INODELOCK_UPDATE);
        if (IS_ERR(parent))
                RETURN(PTR_ERR(parent));

        if (mdt_object_obf(parent))
                GOTO(out_put_parent, rc = -EPERM);

        rc = mdt_version_get_check_save(info, parent, 0);
        if (rc)
                GOTO(out_put_parent, rc);

        /*
         * Check child name version during replay.
         * During create replay a file may exist with same name.
         */
        lname = mdt_name(info->mti_env, (char *)rr->rr_name, rr->rr_namelen);
        rc = mdt_lookup_version_check(info, parent, lname,
                                      &info->mti_tmp_fid1, 1);
        /* -ENOENT is expected here */
        if (rc != 0 && rc != -ENOENT)
                GOTO(out_put_parent, rc);

        /* save version of file name for replay, it must be ENOENT here */
        mdt_enoent_version_save(info, 1);

        child = mdt_object_find(info->mti_env, mdt, rr->rr_fid2);
        if (likely(!IS_ERR(child))) {
                struct md_object *next = mdt_object_child(parent);

                ma->ma_need = MA_INODE;
                ma->ma_valid = 0;
                /* capa for cross-ref will be stored here */
                ma->ma_capa = req_capsule_server_get(info->mti_pill,
                                                     &RMF_CAPA1);
                LASSERT(ma->ma_capa);

                mdt_fail_write(info->mti_env, info->mti_mdt->mdt_bottom,
                               OBD_FAIL_MDS_REINT_CREATE_WRITE);

                /* Version of child will be updated on disk. */
                info->mti_mos = child;
                rc = mdt_version_get_check_save(info, child, 2);
                if (rc)
                        GOTO(out_put_child, rc);

                /* Let lower layer know current lock mode. */
                info->mti_spec.sp_cr_mode =
                        mdt_dlm_mode2mdl_mode(lh->mlh_pdo_mode);

                /*
                 * Do perform lookup sanity check. We do not know if name exists
                 * or not.
                 */
                info->mti_spec.sp_cr_lookup = 1;
                info->mti_spec.sp_feat = &dt_directory_features;

                rc = mdo_create(info->mti_env, next, lname,
                                mdt_object_child(child),
                                &info->mti_spec, ma);
                if (rc == 0) {
                        /* Return fid & attr to client. */
                        if (ma->ma_valid & MA_INODE)
                                mdt_pack_attr2body(info, repbody, &ma->ma_attr,
                                                   mdt_object_fid(child));
                }
out_put_child:
                mdt_object_put(info->mti_env, child);
        } else {
                rc = PTR_ERR(child);
        }
        mdt_create_pack_capa(info, rc, child, repbody);
out_put_parent:
        mdt_object_unlock_put(info, parent, lh, rc);
        RETURN(rc);
}

/* Partial request to create object only */
static int mdt_md_mkobj(struct mdt_thread_info *info)
{
        struct mdt_device      *mdt = info->mti_mdt;
        struct mdt_object      *o;
        struct mdt_body        *repbody;
        struct md_attr         *ma = &info->mti_attr;
        int rc;
        ENTRY;

        DEBUG_REQ(D_INODE, mdt_info_req(info), "Partial create "DFID"",
                  PFID(info->mti_rr.rr_fid2));

        repbody = req_capsule_server_get(info->mti_pill, &RMF_MDT_BODY);

        o = mdt_object_find(info->mti_env, mdt, info->mti_rr.rr_fid2);
        if (!IS_ERR(o)) {
                struct md_object *next = mdt_object_child(o);

                ma->ma_need = MA_INODE;
                ma->ma_valid = 0;

                /*
                 * Cross-ref create can encounter already created obj in case of
                 * recovery, just get attr in that case.
                 */
                if (mdt_object_exists(o) == 1) {
                        rc = mo_attr_get(info->mti_env, next, ma);
                } else {
                        /*
                         * Here, NO permission check for object_create,
                         * such check has been done on the original MDS.
                         */
                        rc = mo_object_create(info->mti_env, next,
                                              &info->mti_spec, ma);
                }
                if (rc == 0) {
                        /* Return fid & attr to client. */
                        if (ma->ma_valid & MA_INODE)
                                mdt_pack_attr2body(info, repbody, &ma->ma_attr,
                                                   mdt_object_fid(o));
                }
                mdt_object_put(info->mti_env, o);
        } else
                rc = PTR_ERR(o);

        mdt_create_pack_capa(info, rc, o, repbody);
        RETURN(rc);
}

int mdt_attr_set(struct mdt_thread_info *info, struct mdt_object *mo,
                 struct md_attr *ma, int flags)
{
        struct mdt_lock_handle  *lh;
        int do_vbr = ma->ma_attr.la_valid & (LA_MODE|LA_UID|LA_GID|LA_FLAGS);
        __u64 lockpart = MDS_INODELOCK_UPDATE;
        int rc;
        ENTRY;

        /* attr shouldn't be set on remote object */
        LASSERT(mdt_object_exists(mo) >= 0);

        lh = &info->mti_lh[MDT_LH_PARENT];
        mdt_lock_reg_init(lh, LCK_PW);

        if (ma->ma_attr.la_valid & (LA_MODE|LA_UID|LA_GID))
                lockpart |= MDS_INODELOCK_LOOKUP;

        rc = mdt_object_lock(info, mo, lh, lockpart, MDT_LOCAL_LOCK);
        if (rc != 0)
                RETURN(rc);

        if (mdt_object_exists(mo) == 0)
                GOTO(out_unlock, rc = -ENOENT);

        /* all attrs are packed into mti_attr in unpack_setattr */
        mdt_fail_write(info->mti_env, info->mti_mdt->mdt_bottom,
                       OBD_FAIL_MDS_REINT_SETATTR_WRITE);

        /* This is only for set ctime when rename's source is on remote MDS. */
        if (unlikely(ma->ma_attr.la_valid == LA_CTIME))
                ma->ma_attr_flags |= MDS_VTX_BYPASS;

        /* VBR: update version if attr changed are important for recovery */
        if (do_vbr) {
                /* update on-disk version of changed object */
                info->mti_mos = mo;
                rc = mdt_version_get_check_save(info, mo, 0);
                if (rc)
                        GOTO(out_unlock, rc);
        }

        /* all attrs are packed into mti_attr in unpack_setattr */
        rc = mo_attr_set(info->mti_env, mdt_object_child(mo), ma);
        if (rc != 0)
                GOTO(out_unlock, rc);

        EXIT;
out_unlock:
        mdt_object_unlock(info, mo, lh, rc);
        return rc;
}

static int mdt_reint_setattr(struct mdt_thread_info *info,
                             struct mdt_lock_handle *lhc)
{
        struct md_attr          *ma = &info->mti_attr;
        struct mdt_reint_record *rr = &info->mti_rr;
        struct ptlrpc_request   *req = mdt_info_req(info);
        struct mdt_export_data  *med = &req->rq_export->exp_mdt_data;
        struct mdt_file_data    *mfd;
        struct mdt_object       *mo;
        struct md_object        *next;
        struct mdt_body         *repbody;
        int                      som_au, rc;
        ENTRY;

        DEBUG_REQ(D_INODE, req, "setattr "DFID" %x", PFID(rr->rr_fid1),
                  (unsigned int)ma->ma_attr.la_valid);

        if (info->mti_dlm_req)
                ldlm_request_cancel(req, info->mti_dlm_req, 0);

        repbody = req_capsule_server_get(info->mti_pill, &RMF_MDT_BODY);
        mo = mdt_object_find(info->mti_env, info->mti_mdt, rr->rr_fid1);
        if (IS_ERR(mo))
                GOTO(out, rc = PTR_ERR(mo));

	if (mdt_object_obf(mo))
		GOTO(out_put, rc = -EPERM);

        /* start a log jounal handle if needed */
        if (!(mdt_conn_flags(info) & OBD_CONNECT_SOM)) {
                if ((ma->ma_attr.la_valid & LA_SIZE) ||
                    (rr->rr_flags & MRF_OPEN_TRUNC)) {
                        /* Check write access for the O_TRUNC case */
                        if (mdt_write_read(mo) < 0)
                                GOTO(out_put, rc = -ETXTBSY);
                }
        } else if (info->mti_ioepoch &&
                   (info->mti_ioepoch->flags & MF_EPOCH_OPEN)) {
                /* Truncate case. IOEpoch is opened. */
                rc = mdt_write_get(mo);
                if (rc)
                        GOTO(out_put, rc);

                mfd = mdt_mfd_new();
                if (mfd == NULL) {
                        mdt_write_put(mo);
                        GOTO(out_put, rc = -ENOMEM);
                }

                mdt_ioepoch_open(info, mo, 0);
                repbody->ioepoch = mo->mot_ioepoch;

                mdt_object_get(info->mti_env, mo);
                mdt_mfd_set_mode(mfd, FMODE_TRUNC);
                mfd->mfd_object = mo;
                mfd->mfd_xid = req->rq_xid;

                cfs_spin_lock(&med->med_open_lock);
                cfs_list_add(&mfd->mfd_list, &med->med_open_head);
                cfs_spin_unlock(&med->med_open_lock);
                repbody->handle.cookie = mfd->mfd_handle.h_cookie;
        }

        som_au = info->mti_ioepoch && info->mti_ioepoch->flags & MF_SOM_CHANGE;
        if (som_au) {
                /* SOM Attribute update case. Find the proper mfd and update
                 * SOM attributes on the proper object. */
                LASSERT(mdt_conn_flags(info) & OBD_CONNECT_SOM);
                LASSERT(info->mti_ioepoch);

                cfs_spin_lock(&med->med_open_lock);
                mfd = mdt_handle2mfd(info, &info->mti_ioepoch->handle);
                if (mfd == NULL) {
                        cfs_spin_unlock(&med->med_open_lock);
                        CDEBUG(D_INODE, "no handle for file close: "
                               "fid = "DFID": cookie = "LPX64"\n",
                               PFID(info->mti_rr.rr_fid1),
                               info->mti_ioepoch->handle.cookie);
                        GOTO(out_put, rc = -ESTALE);
                }
                LASSERT(mfd->mfd_mode == FMODE_SOM);
                LASSERT(!(info->mti_ioepoch->flags & MF_EPOCH_CLOSE));

                class_handle_unhash(&mfd->mfd_handle);
                cfs_list_del_init(&mfd->mfd_list);
                cfs_spin_unlock(&med->med_open_lock);

                /* Close the found mfd, update attributes. */
                ma->ma_lmm_size = info->mti_mdt->mdt_max_mdsize;
                OBD_ALLOC_LARGE(ma->ma_lmm, info->mti_mdt->mdt_max_mdsize);
                if (ma->ma_lmm == NULL)
                        GOTO(out_put, rc = -ENOMEM);

                mdt_mfd_close(info, mfd);

                OBD_FREE_LARGE(ma->ma_lmm, info->mti_mdt->mdt_max_mdsize);
        } else {
                rc = mdt_attr_set(info, mo, ma, rr->rr_flags);
                if (rc)
                        GOTO(out_put, rc);
        }

        ma->ma_need = MA_INODE;
        ma->ma_valid = 0;
        next = mdt_object_child(mo);
        rc = mo_attr_get(info->mti_env, next, ma);
        if (rc != 0)
                GOTO(out_put, rc);

        mdt_pack_attr2body(info, repbody, &ma->ma_attr, mdt_object_fid(mo));

        if (info->mti_mdt->mdt_opts.mo_oss_capa &&
            info->mti_exp->exp_connect_data.ocd_connect_flags & OBD_CONNECT_OSS_CAPA &&
            S_ISREG(lu_object_attr(&mo->mot_obj.mo_lu)) &&
            (ma->ma_attr.la_valid & LA_SIZE) && !som_au) {
                struct lustre_capa *capa;

                capa = req_capsule_server_get(info->mti_pill, &RMF_CAPA2);
                LASSERT(capa);
                capa->lc_opc = CAPA_OPC_OSS_DEFAULT | CAPA_OPC_OSS_TRUNC;
                rc = mo_capa_get(info->mti_env, mdt_object_child(mo), capa, 0);
                if (rc)
                        GOTO(out_put, rc);
                repbody->valid |= OBD_MD_FLOSSCAPA;
        }

        EXIT;
out_put:
        mdt_object_put(info->mti_env, mo);
out:
        if (rc == 0)
                mdt_counter_incr(req->rq_export, LPROC_MDT_SETATTR);

        mdt_shrink_reply(info);
        return rc;
}

static int mdt_reint_create(struct mdt_thread_info *info,
                            struct mdt_lock_handle *lhc)
{
        struct ptlrpc_request   *req = mdt_info_req(info);
        int                     rc;
        ENTRY;

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_REINT_CREATE))
                RETURN(err_serious(-ESTALE));

        if (info->mti_dlm_req)
                ldlm_request_cancel(mdt_info_req(info), info->mti_dlm_req, 0);

        switch (info->mti_attr.ma_attr.la_mode & S_IFMT) {
        case S_IFDIR:{
                /* Cross-ref case. */
                /* TODO: we can add LPROC_MDT_CROSS for cross-ref stats */
                if (info->mti_cross_ref) {
                        rc = mdt_md_mkobj(info);
                } else {
                        LASSERT(info->mti_rr.rr_namelen > 0);
                        mdt_counter_incr(req->rq_export, LPROC_MDT_MKDIR);
                        rc = mdt_md_create(info);
                }
                break;
        }
        case S_IFREG:
        case S_IFLNK:
        case S_IFCHR:
        case S_IFBLK:
        case S_IFIFO:
        case S_IFSOCK:{
                /* Special file should stay on the same node as parent. */
                LASSERT(info->mti_rr.rr_namelen > 0);
                mdt_counter_incr(req->rq_export, LPROC_MDT_MKNOD);
                rc = mdt_md_create(info);
                break;
        }
        default:
                rc = err_serious(-EOPNOTSUPP);
        }
        RETURN(rc);
}

/*
 * VBR: save parent version in reply and child version getting by its name.
 * Version of child is getting and checking during its lookup. If
 */
static int mdt_reint_unlink(struct mdt_thread_info *info,
                            struct mdt_lock_handle *lhc)
{
        struct mdt_reint_record *rr = &info->mti_rr;
        struct ptlrpc_request   *req = mdt_info_req(info);
        struct md_attr          *ma = &info->mti_attr;
        struct lu_fid           *child_fid = &info->mti_tmp_fid1;
        struct mdt_object       *mp;
        struct mdt_object       *mc;
        struct mdt_lock_handle  *parent_lh;
        struct mdt_lock_handle  *child_lh;
        struct lu_name          *lname;
        int                      rc;
        ENTRY;

        DEBUG_REQ(D_INODE, req, "unlink "DFID"/%s", PFID(rr->rr_fid1),
                  rr->rr_name);

        if (info->mti_dlm_req)
                ldlm_request_cancel(req, info->mti_dlm_req, 0);

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_REINT_UNLINK))
                RETURN(err_serious(-ENOENT));

        /*
         * step 1: lock the parent. Note, this may be child in case of
         * remote operation denoted by ->mti_cross_ref flag.
         */
        parent_lh = &info->mti_lh[MDT_LH_PARENT];
        if (info->mti_cross_ref) {
                /*
                 * Init reg lock for cross ref case when we need to do only
                 * ref del locally.
                 */
                mdt_lock_reg_init(parent_lh, LCK_PW);
        } else {
                mdt_lock_pdo_init(parent_lh, LCK_PW, rr->rr_name,
                                  rr->rr_namelen);
        }
        mp = mdt_object_find_lock(info, rr->rr_fid1, parent_lh,
                                  MDS_INODELOCK_UPDATE);
        if (IS_ERR(mp)) {
                rc = PTR_ERR(mp);
                /* errors are possible here in cross-ref cases, see below */
                if (info->mti_cross_ref)
                        rc = 0;
                GOTO(out, rc);
        }

        if (mdt_object_obf(mp))
                GOTO(out_unlock_parent, rc = -EPERM);

        rc = mdt_version_get_check_save(info, mp, 0);
        if (rc)
                GOTO(out_unlock_parent, rc);

        mdt_reint_init_ma(info, ma);
        if (!ma->ma_lmm || !ma->ma_cookie)
                GOTO(out_unlock_parent, rc = -EINVAL);

        if (info->mti_cross_ref) {
                /*
                 * Remote partial operation. It is possible that replay may
                 * happen on parent MDT and this operation will be repeated.
                 * Therefore the object absense is allowed case and nothing
                 * should be done here.
                 */
                if (mdt_object_exists(mp) > 0) {
                        mdt_set_capainfo(info, 0, rr->rr_fid1, BYPASS_CAPA);
                        rc = mo_ref_del(info->mti_env,
                                        mdt_object_child(mp), ma);
                        if (rc == 0)
                                mdt_handle_last_unlink(info, mp, ma);
                } else
                        rc = 0;
                GOTO(out_unlock_parent, rc);
        }

        /* step 2: find & lock the child */
        lname = mdt_name(info->mti_env, (char *)rr->rr_name, rr->rr_namelen);
        /* lookup child object along with version checking */
        fid_zero(child_fid);
        rc = mdt_lookup_version_check(info, mp, lname, child_fid, 1);
        if (rc != 0)
                 GOTO(out_unlock_parent, rc);

        /* We will lock the child regardless it is local or remote. No harm. */
        mc = mdt_object_find(info->mti_env, info->mti_mdt, child_fid);
        if (IS_ERR(mc))
                GOTO(out_unlock_parent, rc = PTR_ERR(mc));
        child_lh = &info->mti_lh[MDT_LH_CHILD];
        mdt_lock_reg_init(child_lh, LCK_EX);
        rc = mdt_object_lock(info, mc, child_lh, MDS_INODELOCK_FULL,
                             MDT_CROSS_LOCK);
        if (rc != 0) {
                mdt_object_put(info->mti_env, mc);
                GOTO(out_unlock_parent, rc);
        }

        mdt_fail_write(info->mti_env, info->mti_mdt->mdt_bottom,
                       OBD_FAIL_MDS_REINT_UNLINK_WRITE);
        /* save version when object is locked */
        mdt_version_get_save(info, mc, 1);
        /*
         * Now we can only make sure we need MA_INODE, in mdd layer, will check
         * whether need MA_LOV and MA_COOKIE.
         */
        ma->ma_need = MA_INODE;
        ma->ma_valid = 0;
        mdt_set_capainfo(info, 1, child_fid, BYPASS_CAPA);
        rc = mdo_unlink(info->mti_env, mdt_object_child(mp),
                        mdt_object_child(mc), lname, ma);
        if (rc == 0)
                mdt_handle_last_unlink(info, mc, ma);

        if (ma->ma_valid & MA_INODE) {
                switch (ma->ma_attr.la_mode & S_IFMT) {
                case S_IFDIR:
                        mdt_counter_incr(req->rq_export, LPROC_MDT_RMDIR);
                        break;
                case S_IFREG:
                case S_IFLNK:
                case S_IFCHR:
                case S_IFBLK:
                case S_IFIFO:
                case S_IFSOCK:
                        mdt_counter_incr(req->rq_export, LPROC_MDT_UNLINK);
                        break;
                default:
                        LASSERTF(0, "bad file type %o unlinking\n",
                                 ma->ma_attr.la_mode);
                }
        }

        EXIT;

        mdt_object_unlock_put(info, mc, child_lh, rc);
out_unlock_parent:
        mdt_object_unlock_put(info, mp, parent_lh, rc);
out:
        return rc;
}

/*
 * VBR: save versions in reply: 0 - parent; 1 - child by fid; 2 - target by
 * name.
 */
static int mdt_reint_link(struct mdt_thread_info *info,
                          struct mdt_lock_handle *lhc)
{
        struct mdt_reint_record *rr = &info->mti_rr;
        struct ptlrpc_request   *req = mdt_info_req(info);
        struct md_attr          *ma = &info->mti_attr;
        struct mdt_object       *ms;
        struct mdt_object       *mp;
        struct mdt_lock_handle  *lhs;
        struct mdt_lock_handle  *lhp;
        struct lu_name          *lname;
        int rc;
        ENTRY;

        DEBUG_REQ(D_INODE, req, "link "DFID" to "DFID"/%s",
                  PFID(rr->rr_fid1), PFID(rr->rr_fid2), rr->rr_name);

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_REINT_LINK))
                RETURN(err_serious(-ENOENT));

        if (info->mti_dlm_req)
                ldlm_request_cancel(req, info->mti_dlm_req, 0);

        if (info->mti_cross_ref) {
                /* MDT holding name ask us to add ref. */
                lhs = &info->mti_lh[MDT_LH_CHILD];
                mdt_lock_reg_init(lhs, LCK_EX);
                ms = mdt_object_find_lock(info, rr->rr_fid1, lhs,
                                          MDS_INODELOCK_UPDATE);
                if (IS_ERR(ms))
                        RETURN(PTR_ERR(ms));

                mdt_set_capainfo(info, 0, rr->rr_fid1, BYPASS_CAPA);
                rc = mo_ref_add(info->mti_env, mdt_object_child(ms), ma);
                mdt_object_unlock_put(info, ms, lhs, rc);
                RETURN(rc);
        }

        /* Invalid case so return error immediately instead of
         * processing it */
        if (lu_fid_eq(rr->rr_fid1, rr->rr_fid2))
                RETURN(-EPERM);

        /* step 1: find & lock the target parent dir */
        lhp = &info->mti_lh[MDT_LH_PARENT];
        mdt_lock_pdo_init(lhp, LCK_PW, rr->rr_name,
                          rr->rr_namelen);
        mp = mdt_object_find_lock(info, rr->rr_fid2, lhp,
                                  MDS_INODELOCK_UPDATE);
        if (IS_ERR(mp))
                RETURN(PTR_ERR(mp));

        if (mdt_object_obf(mp))
                GOTO(out_unlock_parent, rc = -EPERM);

        rc = mdt_version_get_check_save(info, mp, 0);
        if (rc)
                GOTO(out_unlock_parent, rc);

        /* step 2: find & lock the source */
        lhs = &info->mti_lh[MDT_LH_CHILD];
        mdt_lock_reg_init(lhs, LCK_EX);

        ms = mdt_object_find(info->mti_env, info->mti_mdt, rr->rr_fid1);
        if (IS_ERR(ms))
                GOTO(out_unlock_parent, rc = PTR_ERR(ms));

        rc = mdt_object_lock(info, ms, lhs, MDS_INODELOCK_UPDATE,
                            MDT_CROSS_LOCK);
        if (rc != 0) {
                mdt_object_put(info->mti_env, ms);
                GOTO(out_unlock_parent, rc);
        }

        /* step 3: link it */
        mdt_fail_write(info->mti_env, info->mti_mdt->mdt_bottom,
                       OBD_FAIL_MDS_REINT_LINK_WRITE);

        info->mti_mos = ms;
        rc = mdt_version_get_check_save(info, ms, 1);
        if (rc)
                GOTO(out_unlock_child, rc);

        lname = mdt_name(info->mti_env, (char *)rr->rr_name, rr->rr_namelen);
        /** check target version by name during replay */
        rc = mdt_lookup_version_check(info, mp, lname, &info->mti_tmp_fid1, 2);
        if (rc != 0 && rc != -ENOENT)
                GOTO(out_unlock_child, rc);
        /* save version of file name for replay, it must be ENOENT here */
        if (!req_is_replay(mdt_info_req(info))) {
                info->mti_ver[2] = ENOENT_VERSION;
                mdt_version_save(mdt_info_req(info), info->mti_ver[2], 2);
        }

        rc = mdo_link(info->mti_env, mdt_object_child(mp),
                      mdt_object_child(ms), lname, ma);

        if (rc == 0)
                mdt_counter_incr(req->rq_export, LPROC_MDT_LINK);

        EXIT;
out_unlock_child:
        mdt_object_unlock_put(info, ms, lhs, rc);
out_unlock_parent:
        mdt_object_unlock_put(info, mp, lhp, rc);
        return rc;
}
/**
 * lock the part of the directory according to the hash of the name
 * (lh->mlh_pdo_hash) in parallel directory lock.
 */
static int mdt_pdir_hash_lock(struct mdt_thread_info *info,
                       struct mdt_lock_handle *lh,
                       struct mdt_object *obj, __u64 ibits)
{
        struct ldlm_res_id *res_id = &info->mti_res_id;
        struct ldlm_namespace *ns = info->mti_mdt->mdt_namespace;
        ldlm_policy_data_t *policy = &info->mti_policy;
        int rc;

        /*
         * Finish res_id initializing by name hash marking part of
         * directory which is taking modification.
         */
        LASSERT(lh->mlh_pdo_hash != 0);
        fid_build_pdo_res_name(mdt_object_fid(obj), lh->mlh_pdo_hash, res_id);
        memset(policy, 0, sizeof(*policy));
        policy->l_inodebits.bits = ibits;
        /*
         * Use LDLM_FL_LOCAL_ONLY for this lock. We do not know yet if it is
         * going to be sent to client. If it is - mdt_intent_policy() path will
         * fix it up and turn FL_LOCAL flag off.
         */
        rc = mdt_fid_lock(ns, &lh->mlh_reg_lh, lh->mlh_reg_mode, policy,
                          res_id, LDLM_FL_LOCAL_ONLY | LDLM_FL_ATOMIC_CB,
                          &info->mti_exp->exp_handle.h_cookie);
        return rc;
}

/* partial operation for rename */
static int mdt_reint_rename_tgt(struct mdt_thread_info *info)
{
        struct mdt_reint_record *rr = &info->mti_rr;
        struct ptlrpc_request   *req = mdt_info_req(info);
        struct md_attr          *ma = &info->mti_attr;
        struct mdt_object       *mtgtdir;
        struct mdt_object       *mtgt = NULL;
        struct mdt_lock_handle  *lh_tgtdir;
        struct mdt_lock_handle  *lh_tgt = NULL;
        struct lu_fid           *tgt_fid = &info->mti_tmp_fid1;
        struct lu_name          *lname;
        int                      rc;
        ENTRY;

        DEBUG_REQ(D_INODE, req, "rename_tgt: insert (%s->"DFID") in "DFID,
                  rr->rr_tgt, PFID(rr->rr_fid2), PFID(rr->rr_fid1));

        /* step 1: lookup & lock the tgt dir. */
        lh_tgtdir = &info->mti_lh[MDT_LH_PARENT];
        mdt_lock_pdo_init(lh_tgtdir, LCK_PW, rr->rr_tgt,
                          rr->rr_tgtlen);
        mtgtdir = mdt_object_find_lock(info, rr->rr_fid1, lh_tgtdir,
                                       MDS_INODELOCK_UPDATE);
        if (IS_ERR(mtgtdir))
                RETURN(PTR_ERR(mtgtdir));

        /* step 2: find & lock the target object if exists. */
        mdt_set_capainfo(info, 0, rr->rr_fid1, BYPASS_CAPA);
        lname = mdt_name(info->mti_env, (char *)rr->rr_tgt, rr->rr_tgtlen);
        rc = mdo_lookup(info->mti_env, mdt_object_child(mtgtdir),
                        lname, tgt_fid, &info->mti_spec);
        if (rc != 0 && rc != -ENOENT) {
                GOTO(out_unlock_tgtdir, rc);
        } else if (rc == 0) {
                /*
                 * In case of replay that name can be already inserted, check
                 * that and do nothing if so.
                 */
                if (lu_fid_eq(tgt_fid, rr->rr_fid2))
                        GOTO(out_unlock_tgtdir, rc);

                lh_tgt = &info->mti_lh[MDT_LH_CHILD];
                mdt_lock_reg_init(lh_tgt, LCK_EX);

                mtgt = mdt_object_find_lock(info, tgt_fid, lh_tgt,
                                            MDS_INODELOCK_LOOKUP);
                if (IS_ERR(mtgt))
                        GOTO(out_unlock_tgtdir, rc = PTR_ERR(mtgt));

                mdt_reint_init_ma(info, ma);
                if (!ma->ma_lmm || !ma->ma_cookie)
                        GOTO(out_unlock_tgt, rc = -EINVAL);

                rc = mdo_rename_tgt(info->mti_env, mdt_object_child(mtgtdir),
                                    mdt_object_child(mtgt), rr->rr_fid2,
                                    lname, ma);
        } else /* -ENOENT */ {
                rc = mdo_name_insert(info->mti_env, mdt_object_child(mtgtdir),
                                     lname, rr->rr_fid2, ma);
        }

        /* handle last link of tgt object */
        if (rc == 0 && mtgt)
                mdt_handle_last_unlink(info, mtgt, ma);

        EXIT;
out_unlock_tgt:
        if (mtgt)
                mdt_object_unlock_put(info, mtgt, lh_tgt, rc);
out_unlock_tgtdir:
        mdt_object_unlock_put(info, mtgtdir, lh_tgtdir, rc);
        return rc;
}

static int mdt_rename_lock(struct mdt_thread_info *info,
                           struct lustre_handle *lh)
{
        struct ldlm_namespace *ns     = info->mti_mdt->mdt_namespace;
        ldlm_policy_data_t    *policy = &info->mti_policy;
        struct ldlm_res_id    *res_id = &info->mti_res_id;
	__u64                  flags = 0;
        struct md_site        *ms;
        int rc;
        ENTRY;

        ms = mdt_md_site(info->mti_mdt);
        fid_build_reg_res_name(&LUSTRE_BFL_FID, res_id);

        memset(policy, 0, sizeof *policy);
        policy->l_inodebits.bits = MDS_INODELOCK_UPDATE;

        if (ms->ms_control_exp == NULL) {
		flags = LDLM_FL_LOCAL_ONLY | LDLM_FL_ATOMIC_CB;

                /*
                 * Current node is controller, that is mdt0, where we should
                 * take BFL lock.
                 */
                rc = ldlm_cli_enqueue_local(ns, res_id, LDLM_IBITS, policy,
                                            LCK_EX, &flags, ldlm_blocking_ast,
                                            ldlm_completion_ast, NULL, NULL, 0,
                                            &info->mti_exp->exp_handle.h_cookie,
                                            lh);
        } else {
                struct ldlm_enqueue_info einfo = { LDLM_IBITS, LCK_EX,
                     ldlm_blocking_ast, ldlm_completion_ast, NULL, NULL, NULL };
                /*
                 * This is the case mdt0 is remote node, issue DLM lock like
                 * other clients.
                 */
                rc = ldlm_cli_enqueue(ms->ms_control_exp, NULL, &einfo, res_id,
                                      policy, &flags, NULL, 0, lh, 0);
        }

        RETURN(rc);
}

static void mdt_rename_unlock(struct lustre_handle *lh)
{
        ENTRY;
        LASSERT(lustre_handle_is_used(lh));
        ldlm_lock_decref(lh, LCK_EX);
        EXIT;
}

/*
 * This is is_subdir() variant, it is CMD if cmm forwards it to correct
 * target. Source should not be ancestor of target dir. May be other rename
 * checks can be moved here later.
 */
static int mdt_rename_sanity(struct mdt_thread_info *info, struct lu_fid *fid)
{
        struct mdt_reint_record *rr = &info->mti_rr;
        struct lu_fid dst_fid = *rr->rr_fid2;
        struct mdt_object *dst;
        int rc = 0;
        ENTRY;

        do {
                LASSERT(fid_is_sane(&dst_fid));
                dst = mdt_object_find(info->mti_env, info->mti_mdt, &dst_fid);
                if (!IS_ERR(dst)) {
                        rc = mdo_is_subdir(info->mti_env,
                                           mdt_object_child(dst), fid,
                                           &dst_fid);
                        mdt_object_put(info->mti_env, dst);
                        if (rc != -EREMOTE && rc < 0) {
                                CERROR("Failed mdo_is_subdir(), rc %d\n", rc);
                        } else {
                                /* check the found fid */
                                if (lu_fid_eq(&dst_fid, fid))
                                        rc = -EINVAL;
                        }
                } else {
                        rc = PTR_ERR(dst);
                }
        } while (rc == -EREMOTE);

        RETURN(rc);
}

/*
 * VBR: rename versions in reply: 0 - src parent; 1 - tgt parent;
 * 2 - src child; 3 - tgt child.
 * Update on disk version of src child.
 */
static int mdt_reint_rename(struct mdt_thread_info *info,
                            struct mdt_lock_handle *lhc)
{
        struct mdt_reint_record *rr = &info->mti_rr;
        struct md_attr          *ma = &info->mti_attr;
        struct ptlrpc_request   *req = mdt_info_req(info);
        struct mdt_object       *msrcdir;
        struct mdt_object       *mtgtdir;
        struct mdt_object       *mold;
        struct mdt_object       *mnew = NULL;
        struct mdt_lock_handle  *lh_srcdirp;
        struct mdt_lock_handle  *lh_tgtdirp;
        struct mdt_lock_handle  *lh_oldp;
        struct mdt_lock_handle  *lh_newp;
        struct lu_fid           *old_fid = &info->mti_tmp_fid1;
        struct lu_fid           *new_fid = &info->mti_tmp_fid2;
        struct lustre_handle     rename_lh = { 0 };
        struct lu_name           slname = { 0 };
        struct lu_name          *lname;
        int                      rc;
        ENTRY;

        if (info->mti_dlm_req)
                ldlm_request_cancel(req, info->mti_dlm_req, 0);

        if (info->mti_cross_ref) {
                rc = mdt_reint_rename_tgt(info);
                RETURN(rc);
        }

        DEBUG_REQ(D_INODE, req, "rename "DFID"/%s to "DFID"/%s",
                  PFID(rr->rr_fid1), rr->rr_name,
                  PFID(rr->rr_fid2), rr->rr_tgt);

        rc = mdt_rename_lock(info, &rename_lh);
        if (rc) {
                CERROR("Can't lock FS for rename, rc %d\n", rc);
                RETURN(rc);
        }

        lh_newp = &info->mti_lh[MDT_LH_NEW];

        /* step 1: lock the source dir. */
        lh_srcdirp = &info->mti_lh[MDT_LH_PARENT];
        mdt_lock_pdo_init(lh_srcdirp, LCK_PW, rr->rr_name,
                          rr->rr_namelen);
        msrcdir = mdt_object_find_lock(info, rr->rr_fid1, lh_srcdirp,
                                       MDS_INODELOCK_UPDATE);
        if (IS_ERR(msrcdir))
                GOTO(out_rename_lock, rc = PTR_ERR(msrcdir));

        if (mdt_object_obf(msrcdir))
                GOTO(out_unlock_source, rc = -EPERM);

        rc = mdt_version_get_check_save(info, msrcdir, 0);
        if (rc)
                GOTO(out_unlock_source, rc);

        /* step 2: find & lock the target dir. */
        lh_tgtdirp = &info->mti_lh[MDT_LH_CHILD];
        mdt_lock_pdo_init(lh_tgtdirp, LCK_PW, rr->rr_tgt,
                          rr->rr_tgtlen);
        if (lu_fid_eq(rr->rr_fid1, rr->rr_fid2)) {
                mdt_object_get(info->mti_env, msrcdir);
                mtgtdir = msrcdir;
                if (lh_tgtdirp->mlh_pdo_hash != lh_srcdirp->mlh_pdo_hash) {
                         rc = mdt_pdir_hash_lock(info, lh_tgtdirp, mtgtdir,
                                                 MDS_INODELOCK_UPDATE);
                         if (rc)
                                 GOTO(out_unlock_source, rc);
                         OBD_FAIL_TIMEOUT(OBD_FAIL_MDS_PDO_LOCK2, 10);
                }
        } else {
                mtgtdir = mdt_object_find(info->mti_env, info->mti_mdt,
                                          rr->rr_fid2);
                if (IS_ERR(mtgtdir))
                        GOTO(out_unlock_source, rc = PTR_ERR(mtgtdir));

                if (mdt_object_obf(mtgtdir))
                        GOTO(out_put_target, rc = -EPERM);

                /* check early, the real version will be saved after locking */
                rc = mdt_version_get_check(info, mtgtdir, 1);
                if (rc)
                        GOTO(out_put_target, rc);

                rc = mdt_object_exists(mtgtdir);
                if (rc == 0) {
                        GOTO(out_put_target, rc = -ESTALE);
                } else if (rc > 0) {
                        /* we lock the target dir if it is local */
                        rc = mdt_object_lock(info, mtgtdir, lh_tgtdirp,
                                             MDS_INODELOCK_UPDATE,
                                             MDT_LOCAL_LOCK);
                        if (rc != 0)
                                GOTO(out_put_target, rc);
                        /* get and save correct version after locking */
                        mdt_version_get_save(info, mtgtdir, 1);
                }
        }

        /* step 3: find & lock the old object. */
        lname = mdt_name(info->mti_env, (char *)rr->rr_name, rr->rr_namelen);
        mdt_name_copy(&slname, lname);
        fid_zero(old_fid);
        rc = mdt_lookup_version_check(info, msrcdir, &slname, old_fid, 2);
        if (rc != 0)
                GOTO(out_unlock_target, rc);

        if (lu_fid_eq(old_fid, rr->rr_fid1) || lu_fid_eq(old_fid, rr->rr_fid2))
                GOTO(out_unlock_target, rc = -EINVAL);

        mold = mdt_object_find(info->mti_env, info->mti_mdt, old_fid);
        if (IS_ERR(mold))
                GOTO(out_unlock_target, rc = PTR_ERR(mold));

	if (mdt_object_obf(mold)) {
		mdt_object_put(info->mti_env, mold);
		GOTO(out_unlock_target, rc = -EPERM);
	}

        lh_oldp = &info->mti_lh[MDT_LH_OLD];
        mdt_lock_reg_init(lh_oldp, LCK_EX);
        rc = mdt_object_lock(info, mold, lh_oldp, MDS_INODELOCK_LOOKUP,
                             MDT_CROSS_LOCK);
        if (rc != 0) {
                mdt_object_put(info->mti_env, mold);
                GOTO(out_unlock_target, rc);
        }

        info->mti_mos = mold;
        /* save version after locking */
        mdt_version_get_save(info, mold, 2);
        mdt_set_capainfo(info, 2, old_fid, BYPASS_CAPA);

        /* step 4: find & lock the new object. */
        /* new target object may not exist now */
        lname = mdt_name(info->mti_env, (char *)rr->rr_tgt, rr->rr_tgtlen);
        /* lookup with version checking */
        fid_zero(new_fid);
        rc = mdt_lookup_version_check(info, mtgtdir, lname, new_fid, 3);
        if (rc == 0) {
                /* the new_fid should have been filled at this moment */
                if (lu_fid_eq(old_fid, new_fid))
                       GOTO(out_unlock_old, rc);

                if (lu_fid_eq(new_fid, rr->rr_fid1) ||
                    lu_fid_eq(new_fid, rr->rr_fid2))
                        GOTO(out_unlock_old, rc = -EINVAL);

                mdt_lock_reg_init(lh_newp, LCK_EX);
                mnew = mdt_object_find(info->mti_env, info->mti_mdt, new_fid);
                if (IS_ERR(mnew))
                        GOTO(out_unlock_old, rc = PTR_ERR(mnew));

		if (mdt_object_obf(mnew)) {
			mdt_object_put(info->mti_env, mnew);
			GOTO(out_unlock_old, rc = -EPERM);
		}

                rc = mdt_object_lock(info, mnew, lh_newp,
                                     MDS_INODELOCK_FULL, MDT_CROSS_LOCK);
                if (rc != 0) {
                        mdt_object_put(info->mti_env, mnew);
                        GOTO(out_unlock_old, rc);
                }
                /* get and save version after locking */
                mdt_version_get_save(info, mnew, 3);
                mdt_set_capainfo(info, 3, new_fid, BYPASS_CAPA);
        } else if (rc != -EREMOTE && rc != -ENOENT) {
                GOTO(out_unlock_old, rc);
        } else {
                mdt_enoent_version_save(info, 3);
        }

        /* step 5: rename it */
        mdt_reint_init_ma(info, ma);
        if (!ma->ma_lmm || !ma->ma_cookie)
                GOTO(out_unlock_new, rc = -EINVAL);

        mdt_fail_write(info->mti_env, info->mti_mdt->mdt_bottom,
                       OBD_FAIL_MDS_REINT_RENAME_WRITE);


        /* Check if @dst is subdir of @src. */
        rc = mdt_rename_sanity(info, old_fid);
        if (rc)
                GOTO(out_unlock_new, rc);

        rc = mdo_rename(info->mti_env, mdt_object_child(msrcdir),
                        mdt_object_child(mtgtdir), old_fid, &slname,
                        (mnew ? mdt_object_child(mnew) : NULL),
                        lname, ma);

        /* handle last link of tgt object */
        if (rc == 0) {
                mdt_counter_incr(req->rq_export, LPROC_MDT_RENAME);
                if (mnew)
                        mdt_handle_last_unlink(info, mnew, ma);

                mdt_rename_counter_tally(info, info->mti_mdt, req->rq_export,
                                         msrcdir, mtgtdir);
        }

        EXIT;
out_unlock_new:
        if (mnew)
                mdt_object_unlock_put(info, mnew, lh_newp, rc);
out_unlock_old:
        mdt_object_unlock_put(info, mold, lh_oldp, rc);
out_unlock_target:
        mdt_object_unlock(info, mtgtdir, lh_tgtdirp, rc);
out_put_target:
        mdt_object_put(info->mti_env, mtgtdir);
out_unlock_source:
        mdt_object_unlock_put(info, msrcdir, lh_srcdirp, rc);
out_rename_lock:
        mdt_rename_unlock(&rename_lh);
        return rc;
}

typedef int (*mdt_reinter)(struct mdt_thread_info *info,
                           struct mdt_lock_handle *lhc);

static mdt_reinter reinters[REINT_MAX] = {
        [REINT_SETATTR]  = mdt_reint_setattr,
        [REINT_CREATE]   = mdt_reint_create,
        [REINT_LINK]     = mdt_reint_link,
        [REINT_UNLINK]   = mdt_reint_unlink,
        [REINT_RENAME]   = mdt_reint_rename,
        [REINT_OPEN]     = mdt_reint_open,
        [REINT_SETXATTR] = mdt_reint_setxattr
};

int mdt_reint_rec(struct mdt_thread_info *info,
                  struct mdt_lock_handle *lhc)
{
        int rc;
        ENTRY;

        rc = reinters[info->mti_rr.rr_opcode](info, lhc);

        RETURN(rc);
}
