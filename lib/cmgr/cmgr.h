/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 *
 * This is the interface to the locking and cluster manager
 */

typedef struct lvm_response
{
    char  node[255];
    char *response;
    int   status;
    int   len;

} lvm_response_t;

/* The "direct to cluster manager" API */
extern int cluster_request(char cmd, char *node, void *data, int len,
			       lvm_response_t **response, int *num);
extern int cluster_write(char cmd, char *node, void *data, int len);
extern int cluster_free_request(lvm_response_t *response);


/* The "high-level" API */
extern int lock_for_cluster(char scope, char *name, int namelen, int suspend);
extern int unlock_for_cluster(char scope, char *name, int namelen, int suspend);

/* The "even higher-level" API that also copes with
   non-clustered environment. */
extern int lock_lvm(int suspend);
extern int unlock_lvm(void);

extern int lock_lv(struct logical_volume *lv, int suspend);
extern int unlock_lv(struct logical_volume *lv);

extern int lock_vg(struct volume_group *vg, int suspend);
extern int unlock_vg(struct volume_group *vg);

/* The system must be locked by one of the above calls before
   using these */
extern int get_lv_open_count(struct logical_volume *lv, int *open_count);
extern int get_vg_active_count(struct volume_group *vg, int *active_count);

/* Lock modes: these are taken from the IBM DLM,
   but we only support EX! */
#define   LKM_NLMODE      0               /* null lock */
#define   LKM_CRMODE      1               /* concurrent read */
#define   LKM_CWMODE      2               /* concurrent write */
#define   LKM_PRMODE      3               /* protected read */
#define   LKM_PWMODE      4               /* protected write */
#define   LKM_EXMODE      5               /* exclusive */
