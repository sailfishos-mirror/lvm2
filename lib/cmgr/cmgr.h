
typedef struct lvm_response
{
    char  node[255];
    char *response;
    int   status;
    int   len;

} lvm_response_t;

extern int lvm_cluster_request(char cmd, char *node, void *data, int len,
			       lvm_response_t **response, int *num);
extern int lvm_cluster_write(char cmd, char *node, void *data, int len);
extern int lvm_cluster_free_request(lvm_response_t *response);


/* The "high-level" API */
extern int lvm_lock_for_cluster(char scope, char *name, int verbosity);
extern int lvm_unlock_for_cluster(char scope, char *name, int verbosity);
