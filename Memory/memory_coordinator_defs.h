/************************************************************************
*
*   DESCRIPTION
*
*       This file contains VCPU scheduler macros, definitions, and
*       structures
*
***********************************************************************/
#ifndef MEMORY_COORDINATOR_DEFS_H
#define MEMORY_COORDINATOR_DEFS_H

/* Set debug to 1 for printf details of memory stats collected / 0 for no output */
#define MEM_COORD_DEBUG                     1

/* Configurable values used for making memory changes for VMs */
#define MEM_COORD_AVAIL_HOST_LOW_PERCENT    10      /* Host with less avail % than this is considered low in memory */
#define MEM_COORD_AVAIL_HOST_TGT_PERCENT    15      /* Target % of available memory for host */
#define MEM_COORD_AVAIL_VM_LOW_PERCENT      25      /* VM with less avail % than this are considered "deficient" in memory */
#define MEM_COORD_AVAIL_VM_TGT_PERCENT      30      /* VM target percent of available memory */
#define MEM_COORD_AVAIL_VM_HIGH_PERCENT     33      /* VMs with more avail % than this are considered to have "excess" memory */

/* Define status errors */
#define MEM_COORD_CONN_ERROR                -1
#define MEM_COORD_NO_DOMAINS                -2
#define MEM_COORD_DOMAIN_LIST_ERROR         -3
#define MEM_COORD_NOMEM                     -4
#define MEM_COORD_DOMAIN_MEM_ERROR          -5
#define MEM_COORD_HOST_FREE_MEM_ERROR       -6

/* Define kilobyte memory size */
#define MEM_COORD_KB_SIZE                   1024

/* Define number of VM memory stats used by the memory coordinator */
#define MEM_COORD_NUM_STATS                 2

/* Structure to keep track of all the virt library variables / data */
typedef struct VIRT_INFO_STRUCT
{
    virConnectPtr       conn;
    int                 num_domains;
    virDomainPtr *      domain_list;
    unsigned long long  host_free_mem;
    unsigned long       host_total_mem;
    unsigned long       host_tgt_mem;
    unsigned int        high_mem_mask;
    unsigned int        low_mem_mask;

} VIRT_INFO;

/* Structure to keep track of VM memory information */
typedef struct VM_MEM_INFO_STRUCT
{
    unsigned long long  mem_free;
    unsigned long long  mem_total;
    unsigned long       mem_max;
    int                 percent_avail;

} VM_MEM_INFO;

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

#endif /* MEMORY_COORDINATOR_DEFS_H */
