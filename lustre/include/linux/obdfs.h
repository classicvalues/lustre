/* object based disk file system
 * 
 * This software is licensed under the GPL.  See the file COPYING in the
 * top directory of this distribution for details.
 * 
 * Copyright (C), 1999, Stelias Computing Inc
 *
 *
 */


#ifndef _OBDFS_H
#define OBDFS_H
#include <linux/obd_class.h>
#include <linux/list.h>

/* super.c */
void obdfs_read_inode(struct inode *inode);

/* flush.c */
int flushd_init(void);


/* rw.c */
int obdfs_do_writepage(struct inode *, struct page *, int sync);
int obdfs_init_pgrqcache(void);
void obdfs_cleanup_pgrqcache(void);
int obdfs_readpage(struct dentry *dentry, struct page *page);
int obdfs_writepage(struct dentry *dentry, struct page *page);
struct page *obdfs_getpage(struct inode *inode, unsigned long offset, int create, int locked);
int obdfs_write_one_page(struct file *file, struct page *page, unsigned long offset, unsigned long bytes, const char * buf);

/* namei.c */
struct dentry *obdfs_lookup(struct inode * dir, struct dentry *dentry);
int obdfs_create (struct inode * dir, struct dentry * dentry, int mode);
int obdfs_mkdir(struct inode *dir, struct dentry *dentry, int mode);
int obdfs_rmdir(struct inode *dir, struct dentry *dentry);
int obdfs_unlink(struct inode *dir, struct dentry *dentry);
int obdfs_mknod(struct inode *dir, struct dentry *dentry, int mode, int rdev);
int obdfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname);
int obdfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry);
int obdfs_rename(struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry);
/* dir.c */
int obdfs_check_dir_entry (const char * function, struct inode * dir,
			  struct ext2_dir_entry_2 * de,
			  struct page * page,
			   unsigned long offset);
/* symlink.c */
int obdfs_readlink (struct dentry *, char *, int);
struct dentry *obdfs_follow_link(struct dentry *, struct dentry *, unsigned int); 


/* list of all OBDFS super blocks  */
struct list_head obdfs_super_list;

struct obdfs_pgrq {
	struct list_head	 rq_plist;	/* linked list of req's */
	unsigned long            rq_jiffies;
	struct page 		*rq_page;	/* page to be written */
};

#if 0
void obdfs_print_list(struct list_head *page_list) {
	struct list_head *tmp = page_list;

	while ( (tmp = tmp->next) != page_list) {
		struct obdfs_pgrq *pgrq;
		pgrq = list_entry(tmp, struct obdfs_pgrq, rq_plist);
		CDEBUG(D_INODE, "page %p\n", pgrq->rq_page);
	}
}

#endif
inline void obdfs_pgrq_del(struct obdfs_pgrq *pgrq);
int obdfs_do_vec_wr(struct super_block *sb, obd_count num_io, obd_count num_oa,
			   struct obdo **obdos, obd_count *oa_bufs,
			   struct page **pages, char **bufs, obd_size *counts,
			   obd_off *offsets, obd_flag *flags);


struct obdfs_sb_info {
	struct list_head         osi_list; /* list of supers */
	struct obd_conn		 osi_conn;
	struct super_block	*osi_super;
	struct obd_device	*osi_obd;
	struct obd_ops		*osi_ops;     
	ino_t			 osi_rootino; /* which root inode */
	int			 osi_minor;   /* minor of /dev/obdX */
	struct list_head	 osi_inodes;  /* linked list of dirty inodes */
};

struct obdfs_inode_info {
	int		 oi_flags;
	struct list_head oi_inodes;
	struct list_head oi_pages;
	char 		 oi_inline[OBD_INLINESZ];
};

static inline struct list_head *obdfs_iplist(struct inode *inode) 
{
	struct obdfs_inode_info *info = (struct obdfs_inode_info *)&inode->u.generic_ip;

	return &info->oi_pages;
}

static inline struct list_head *obdfs_islist(struct inode *inode) 
{
	struct obdfs_inode_info *info = (struct obdfs_inode_info *)&inode->u.generic_ip;

	return &info->oi_inodes;
}

static inline struct list_head *obdfs_slist(struct inode *inode) {
	struct obdfs_sb_info *sbi = (struct obdfs_sb_info *)(&inode->i_sb->u.generic_sbp);
	return &sbi->osi_inodes;
}

#define OBDFS_INFO(inode) ((struct obdfs_inode_info *)(&(inode)->u.generic_ip))

void obdfs_sysctl_init(void);
void obdfs_sysctl_clean(void);

extern struct file_operations obdfs_file_operations;
extern struct inode_operations obdfs_file_inode_operations;
extern struct inode_operations obdfs_dir_inode_operations;
extern struct inode_operations obdfs_symlink_inode_operations;

static inline int obdfs_has_inline(struct inode *inode)
{
	return (OBDFS_INFO(inode)->oi_flags & OBD_FL_INLINEDATA);
}

static void inline obdfs_from_inode(struct obdo *oa, struct inode *inode)
{
	struct obdfs_inode_info *oinfo = OBDFS_INFO(inode);

	CDEBUG(D_INODE, "inode %ld (%p)\n", inode->i_ino, inode);
	obdo_from_inode(oa, inode);
	if (obdfs_has_inline(inode)) {
		CDEBUG(D_INODE, "inode has inline data\n");
		memcpy(oa->o_inline, oinfo->oi_inline, OBD_INLINESZ);
		oa->o_obdflags |= OBD_FL_INLINEDATA;
		oa->o_valid |= OBD_MD_FLINLINE;
	}
} /* obdfs_from_inode */

static void inline obdfs_to_inode(struct inode *inode, struct obdo *oa)
{
	struct obdfs_inode_info *oinfo = OBDFS_INFO(inode);

	CDEBUG(D_INODE, "inode %ld (%p)\n", inode->i_ino, inode);
	obdo_to_inode(inode, oa);

	if (obdo_has_inline(oa)) {
		CDEBUG(D_INODE, "obdo has inline data\n");
		memcpy(oinfo->oi_inline, oa->o_inline, OBD_INLINESZ);
		oinfo->oi_flags |= OBD_FL_INLINEDATA;
	}
} /* obdfs_to_inode */

#define NOLOCK 0
#define LOCKED 1

#ifdef OPS
#warning "*** WARNING redefining OPS"
#else
#define OPS(sb,op) ((struct obdfs_sb_info *)(& ## sb ## ->u.generic_sbp))->osi_ops->o_ ## op
#define IOPS(inode,op) ((struct obdfs_sb_info *)(& ## inode->i_sb ## ->u.generic_sbp))->osi_ops->o_ ## op
#endif

#ifdef ID
#warning "*** WARNING redefining ID"
#else
#define ID(sb) (&((struct obdfs_sb_info *)( & ## sb ## ->u.generic_sbp))->osi_conn)
#define IID(inode) (&((struct obdfs_sb_info *)( & ## inode->i_sb ## ->u.generic_sbp))->osi_conn)
#endif

#define OBDFS_SUPER_MAGIC 0x4711

#endif

