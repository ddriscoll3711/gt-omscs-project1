/************************************************************************
*
*   DESCRIPTION
*
*       This file contains VCPU scheduler macros, definitions, and
*       structures
*
***********************************************************************/
#ifndef VCPU_SCHEDULER_DEFS_H
#define VCPU_SCHEDULER_DEFS_H

/* Set debug to 1 for printf details of VCPU scheduler stats / 0 for no output */
#define VCPU_SCHEDULER_DEBUG                1

/* Configurable values used for making scheduling decisions for VCPUs */
#define VCPU_SCHEDULER_PCPU_HIGH_THRESHOLD  90      /* PCPU utilization above this % considered "high" */
#define VCPU_SCHEDULER_PCPU_TGT             80      /* PCPU target utilization % */
#define VCPU_SCHEDULER_PCPU_LOW_THRESHOLD   70      /* PCPU utilization below this % considered "low" */

/* Define status errors */
#define VCPU_SCHEDULER_CONN_ERROR           -1
#define VCPU_SCHEDULER_NO_DOMAINS           -2
#define VCPU_SCHEDULER_DOMAIN_LIST_ERROR    -3
#define VCPU_SCHEDULER_NOMEM                -4
#define VCPU_SCHEDULER_DOMAIN_INFO_ERROR    -5
#define VCPU_SCHEDULER_PCPU_IDLE_ERROR      -6

/* Time conversion macros */
#define VCPU_SCHEDULER_SEC_TO_NANOSECS      1000000000

/* Structure to keep track of all the virt library variables / data */
typedef struct VIRT_INFO_STRUCT
{
    virConnectPtr       conn;
    int                 num_domains;
    int                 num_pcpus;
    unsigned int        pcpu_high_mask;
    unsigned int        pcpu_low_mask;
    virNodeCPUStats *   params;
    int                 num_params;
    virDomainPtr *      domain_list;

} VIRT_INFO;

/* Structure to keep track of each VM's stats (CPU utilization, etc) and which
   PCPU the VM is affined to (assumes each VM only has a single VCPU) */
typedef struct VCPU_STATS_STRUCT
{
    virDomainPtr                domain_id;  /* Domain ID which contains this VCPU */
    int                         cpu_util;   /* CPU Utilization for this VCPU in % */
    unsigned long long int      last_time;  /* Last read total CPU time from VM boot */
    struct PCPU_STATS_STRUCT *  pcpu;       /* Pointer to PCPU struct that this VCPU is pinned to */
    struct VCPU_STATS_STRUCT *  next;       /* Pointer to next VCPU pinned to the same PCPU */
    struct VCPU_STATS_STRUCT *  prev;       /* Pointer to previous VCPU pinned to the same PCPU */

} VCPU_STATS;

/* Structure to keep track of PCPU overall utilization given number of
   VCPUs pinned to a given PCPU */
typedef struct PCPU_STATS_STRUCT
{
    int                         id;         /* CPU ID for this PCPU */
    unsigned char               cpumap;     /* CPU map to pin a VCPU exclusively to this PCPU */
    int                         cpu_util;   /* CPU Utilization for this CPU in % */
    unsigned long long          last_time;  /* Last read CPU idle time for PCPU */
    int                         num_pinned; /* Total number of pinned VCPUs for this PCPU */
    struct VCPU_STATS_STRUCT *  head;       /* Head of linked list of VCPUs pinned to this PCPU */

} PCPU_STATS;

/* Macro to get bit position of lowest set bit in a 32-bit value */
#define LOW_BIT_GET32(value32, lowbit_ptr)                          \
        {                                                           \
            /* Initialize lowbit to 0 */                            \
            *lowbit_ptr = 0;                                        \
                                                                    \
            /* Use bitscan forward assembly instruction */          \
            asm volatile(" bsf  %0, %1"                             \
                        : "=r" (*(unsigned int *)lowbit_ptr)        \
                        : "r" ((unsigned int)value32));             \
        }

#endif /* VCPU_SCHEDULER_DEFS_H */
