/* Definitions for CLVMD server and clients */


/* The protocol spoken over the cluster and across the local
   socket
*/

struct clvm_header
{
    unsigned char  cmd;        /* See below */
    unsigned char  flags;      /* See below */
    unsigned short pad;        /* To keep alignment sane */
    unsigned int   clientid;   /* Only used in Daemon->Daemon comms */
    int            status;     /* For replies, whether the request suceeded or not */
    unsigned int   arglen;     /* Length of argument below. if >1500 then it will be passed around the
				  cluster in the system LV */
    char           node[1];    /* Actually a NUL-terminated string, node name, if this is empty then
			          the command is forwarded to all cluster nodes unless FLAG_LOCAL is
			          also set. */
    char           args[1];    /* Arguments for the command follow the node name, This member is only
				  valid if the node name is empty */

} __attribute__((packed));

/* Flags */
#define CLVMD_FLAG_LOCAL        1  /* Only do this on the local node */
#define CLVMD_FLAG_SYSTEMLV     2  /* Data is in system LV under my node name */

/* Name of the local socket to communicate between libclvm and clvmd */
#define CLVMD_SOCKNAME "/var/run/clvmd"

/* Command numbers */
#define CLVMD_CMD_TEST               4

/* Lock/Unlock commands */
#define CLVMD_CMD_LOCK              30
#define CLVMD_CMD_UNLOCK            31

#define CLVMD_CMD_LOCK_SUSPEND      32
#define CLVMD_CMD_UNLOCK_RESUME     33

/* Info Commands */
#define CLVMD_CMD_LVDISPLAY         40
#define CLVMD_CMD_LVCHECK           41
#define CLVMD_CMD_VGDISPLAY         42
#define CLVMD_CMD_VGCHECK           43
