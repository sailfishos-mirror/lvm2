
typedef struct lvm_response
{
    char  node[255];
    char *response;
    int   status;
    int   len;

} lvm_response_t;

extern int cluster_request(char cmd, char *node, void *data, int len,
			       lvm_response_t **response, int *num);
extern int cluster_write(char cmd, char *node, void *data, int len);
extern int cluster_free_request(lvm_response_t *response);


/* The "high-level" API */
extern int lock_for_cluster(char scope, char *name, int suspend, int verbosity);
extern int unlock_for_cluster(char scope, char *name, int suspend, int verbosity);

/* The "even higher-level" API that- copes with
   non-clustered environment. */
extern int lock_lvm(char scope, char *name, int suspend, int verbosity);
extern int unlock_lvm(char scope, char *name, int suspend, int verbosity);
