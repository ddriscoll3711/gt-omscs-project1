/************************************************************************
*
*   DESCRIPTION
*
*       This file contains Memory Coordinator code that evaluates 
*       memory usage by VMs in the system and adjusts VM memory to
*       best accommodate the VMs.
*
*   FUNCTIONS
*
*       main
*
***********************************************************************/

/*****************************/
/* INCLUDE FILES             */
/*****************************/
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include "memory_coordinator_defs.h"

/*****************************/
/* LOCAL FUNCTION PROTOTYPES */
/*****************************/
static int  coordinator(unsigned int cycle_time);
static int  collect_mem_stats(void);
static int  vm_memory_adjust(void);
static int  virt_init(void);
static int  vm_mem_info_init(void);
static void virt_deinit(void);
#if (MEM_COORD_DEBUG == 1)
static void dump_mem_stats(void);
#endif  /* (MEM_COORD_DEBUG == 1) */

/*****************************/
/* LOCAL VARIABLES           */
/*****************************/
static VIRT_INFO            virt_info;
static VM_MEM_INFO *        vm_mem_info;


/*************************************************************************
*
*   FUNCTION
*
*       main
*
*   DESCRIPTION
*
*       C entry function
*
*   INPUTS
*
*       argc                                Number of parameters
*       argv                                Array of parameters
*
*   OUTPUTS
*
*       EXIT_SUCCESS                        Application successfully executed
*       EXIT_FAILURE                        Error in running application
*
*************************************************************************/
int main(int argc, char ** argv)
{
    int                 status = EXIT_FAILURE;
    int                 seconds = 0;


    /* Ensure single argument for time interval passed in */
    if (argc == 2)
    {
        /* Convert string time value into integer */
        seconds = atoi((const char *)argv[1]);
    }

    /* Check if 1st parameter (time interval) is a valid number */
    if (seconds == 0)
    {
        /* Print error / usage */
        fprintf(stderr, "Usage:  %s <time interval>\n\r", argv[0]);
        fprintf(stderr, "        where <time interval> = time, in seconds, between cycles.\n\r");
    }
    else
    {
        /* Initialize the virtualization data */
        status = virt_init();

        /* Ensure virtualization init was successful */
        if (status == EXIT_SUCCESS)
        {
            /* Call VCPU scheduler with cycle time */
            status = coordinator(seconds);

            /* Deinit the virtualization data */
            virt_deinit();
        }

        /* Check if error returned */
        if (status != EXIT_SUCCESS)
        {
            /* Print error */
            fprintf(stderr, "Exit error code = %d\n\r", status);
        }
    }

    /* Return status to caller */
    return (status);
}


/*************************************************************************
*
*   FUNCTION
*
*       coordinator
*
*   DESCRIPTION
*
*       Coordinates memory usage among the VMs within the system
*
*   INPUTS
*
*       cycle_time                          Time, in seconds, to run
*                                           each cycle of the memory coordinator
*
*   OUTPUTS
*
*       EXIT_SUCCESS                        Memory coordination successful
*       Others                              Error during memory coordination
*
*************************************************************************/
static int  coordinator(unsigned int cycle_time)
{
    int                 status = EXIT_SUCCESS;


    /* Loop while no errors and no key hit */
    while (status == EXIT_SUCCESS)
    {
        /* Sleep for specified period of time */
        sleep(cycle_time);

        /* Collect memory stats */
        status = collect_mem_stats();

        /* Ensure VM stats obtained successfully */
        if (status == EXIT_SUCCESS)
        {
            /* Adjust memory assignment for each VM */
            status = vm_memory_adjust();
        }

#if (MEM_COORD_DEBUG == 1)
        /* Dump memory coordinator stats */
        dump_mem_stats();
#endif  /* (MEM_COORD_DEBUG == 1) */
    }

    /* Return status to caller */
    return (status);
}


/*************************************************************************
*
*   FUNCTION
*
*       collect_mem_stats
*
*   DESCRIPTION
*
*       Collects VM stats
*
*   INPUTS
*
*       None
*
*   OUTPUTS
*
*       EXIT_SUCCESS                        VM stats collected
*       Others                              Error trying to collect VM stats
*
*************************************************************************/
static int  collect_mem_stats(void)
{
    int                         index, status = EXIT_SUCCESS;
    virDomainMemoryStatStruct   mem_stats[VIR_DOMAIN_MEMORY_STAT_NR];
    int                         num_stats, num_stats_found;


    /* First update the host memory available */
    virt_info.host_free_mem = (virNodeGetFreeMemory(virt_info.conn) / MEM_COORD_KB_SIZE);

    /* Loop through each VM */
    for (index = 0; (index < virt_info.num_domains) && (status == EXIT_SUCCESS) ; index++)
    {
        /* Get memory stats for this domain */
        num_stats = virDomainMemoryStats(virt_info.domain_list[index], &mem_stats[0], VIR_DOMAIN_MEMORY_STAT_NR, 0);

        /* Check if num stats is greater than 0  */
        if (num_stats > 0)
        {
            /* Clear number of stats found before each loop */
            num_stats_found = 0;

            /* Loop through all stats while number of found stats isn't reached */
            while ((num_stats != 0) && (num_stats_found != MEM_COORD_NUM_STATS))
            {
                /* Decrement number of stats to index into next stats array entry */
                num_stats--;

                /* Check if tag for this stat is one we want to keep */
                switch (mem_stats[num_stats].tag)
                {
                    /* Get balloon size */
                    case VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON:

                        /* Save value in VM Memory info structure */
                        vm_mem_info[index].mem_total = mem_stats[num_stats].val;

                        /* Increment number of stats found */
                        num_stats_found++;

                    break;

                    /* Get domain unused memory */
                    case VIR_DOMAIN_MEMORY_STAT_UNUSED:

                        /* Save value as memory available */
                        vm_mem_info[index].mem_free = mem_stats[num_stats].val;

                        /* Increment number of stats found */
                        num_stats_found++;

                    break;

                    /* Default */
                    default:

                        /* Do nothing - just skip these stats */

                    break;
                }
            }   /* while loop */

            /* Ensure appropriate stats available */
            if (vm_mem_info[index].mem_total > 0)
            {
                /* Calculate percent avail memory in VM */
                vm_mem_info[index].percent_avail = (int)((100 * vm_mem_info[index].mem_free)/vm_mem_info[index].mem_total);

                /* Cap percentage of available memory at 100%
                   NOTE:  Value can exceed 100% due to memory stats changing faster in VM than collected or due to order collected
                          (ie not atomic collection of all stats) */
                vm_mem_info[index].percent_avail = (vm_mem_info[index].percent_avail > 100 ? 100 : vm_mem_info[index].percent_avail);

                /* Check if available memory for this VM is low and size of VM isn't at max */
                if ((vm_mem_info[index].percent_avail < MEM_COORD_AVAIL_VM_LOW_PERCENT) &&
                    (vm_mem_info[index].mem_total < vm_mem_info[index].mem_max))
                {
                    /* Set bit for this VM in low mask */
                    virt_info.low_mem_mask |= (1 << index);
                }
                /* Check if available memory for this VM is high */
                else if (vm_mem_info[index].percent_avail > MEM_COORD_AVAIL_VM_HIGH_PERCENT)
                {
                    /* Set bit for this VM in high mask */
                    virt_info.high_mem_mask |= (1 << index);
                }
            }
        }
        else
        {
            /* Set error for VM memory stats not available */
            status = MEM_COORD_DOMAIN_MEM_ERROR;
        }
    }   /* for loop */

    /* Return status to caller */
    return (status);
}


/*************************************************************************
*
*   FUNCTION
*
*       vm_memory_adjust
*
*   DESCRIPTION
*
*       Adjusts memory allocated for each VM based on demands and available
*       memory on the host
*
*   INPUTS
*
*       None
*
*   OUTPUTS
*
*       EXIT_SUCCESS                        Adjustment of memory for VM
*                                           was successful
*       Other                               Error when adjusting VM memory
*
*************************************************************************/
static int  vm_memory_adjust(void)
{
    int                 index, status = EXIT_SUCCESS;
    unsigned int        host_precent_free;
    unsigned long       mem_adj;


    /* Loop through all VMs that have "high" memory and reclaim it */
    while ((virt_info.high_mem_mask != 0) && (status == EXIT_SUCCESS))
    {
        /* Get index of high memory VM */
        LOW_BIT_GET32(virt_info.high_mem_mask, &index);

        /* Calculate reduced memory size of VM so VM will have configured target available percentage */
        mem_adj = ((vm_mem_info[index].mem_total * (vm_mem_info[index].percent_avail - MEM_COORD_AVAIL_VM_TGT_PERCENT)) / 100);

        /* Decrease VM memory size */
        vm_mem_info[index].mem_total -= mem_adj;

        /* Set new memory size for VM */
        status = virDomainSetMemory(virt_info.domain_list[index], vm_mem_info[index].mem_total);

        /* Clear this VMs bit from high mask */
        virt_info.high_mem_mask &= ~(1 << index);
    }

    /* Loop through all VMs that have "low" memory and try to provide them more memory */
    while ((virt_info.low_mem_mask != 0) && (status == EXIT_SUCCESS))
    {
        /* Get most recent host available memory value */
        virt_info.host_free_mem = (virNodeGetFreeMemory(virt_info.conn) / MEM_COORD_KB_SIZE);

        /* Get index of low memory VM */
        LOW_BIT_GET32(virt_info.low_mem_mask, &index);

        /* Calculate memory increase for VM to bring it to target */
        mem_adj = (vm_mem_info[index].mem_total * (MEM_COORD_AVAIL_VM_TGT_PERCENT - vm_mem_info[index].percent_avail)) / 100;

        /* Calculate percent of host memory that is free / available AFTER adjustment to VM */
        host_precent_free = ((virt_info.host_free_mem - mem_adj) * 100)/virt_info.host_total_mem;

        /* Check if the host is above the configured low memory threshold (after this memory adjustment) */
        if (host_precent_free > MEM_COORD_AVAIL_HOST_LOW_PERCENT)
        {
            /* Increase VM memory size */
            vm_mem_info[index].mem_total += mem_adj;

            /* Limit VM memory to maximum memory size */
            vm_mem_info[index].mem_total =
                    (vm_mem_info[index].mem_total > vm_mem_info[index].mem_max ? vm_mem_info[index].mem_max : vm_mem_info[index].mem_total);

            /* Adjust VM memory */
            status = virDomainSetMemory(virt_info.domain_list[index], vm_mem_info[index].mem_total);

            /* Clear this VMs bit from low mask */
            virt_info.low_mem_mask &= ~(1 << index);
        }
        else
        {
            /* Check if current free memory is less than target memory for host */
            if (virt_info.host_free_mem < virt_info.host_tgt_mem)
            {
                /* Calculate total memory adjustment needed for host to get back to target */
                mem_adj = virt_info.host_tgt_mem - virt_info.host_free_mem;

                /* Loop through each VM */
                for (index = 0; index < virt_info.num_domains ; index++)
                {
                    /* Adjust VM memory based on fair share of memory used by this VM */
                    vm_mem_info[index].mem_total -= ((mem_adj * ((100 * vm_mem_info[index].mem_total) / virt_info.host_total_mem))/100);

                    /* Adjust VM memory ignoring any errors */
                    virDomainSetMemory(virt_info.domain_list[index], vm_mem_info[index].mem_total);
                }

                /* Host has low memory - skip all remaining low memory VM updates */
                virt_info.low_mem_mask = 0;
            }
            else
            {
                /* Clear this VMs bit from low mask */
                virt_info.low_mem_mask &= ~(1 << index);
            }
        }
    }

    /* Return status to caller */
    return (status);
}


/*************************************************************************
*
*   FUNCTION
*
*       virt_init
*
*   DESCRIPTION
*
*       Initializes the virtualization connection and determines number of
*       VMs, number of PCPUs, and sets up appropriate data structures used
*       by the VCPU scheduling code
*
*   INPUTS
*
*       None
*
*   OUTPUTS
*
*       EXIT_SUCCESS                        Virtualization init successful
*       Others                              Problems associated with getting
*                                           virtualization information
*
*************************************************************************/
static int  virt_init(void)
{
    int             status = EXIT_SUCCESS;
    virNodeInfo     info;


    /* Attempt to connect to the hypervisor */
    virt_info.conn = virConnectOpen("qemu:///system");

    /* Check if connection to hypervisor was successful */
    if (virt_info.conn != NULL)
    {
        /* Get list of active domains */
        virt_info.num_domains = virConnectListAllDomains(virt_info.conn,
                                                         &virt_info.domain_list,
                                                         VIR_CONNECT_LIST_DOMAINS_ACTIVE);

        /* Ensure at least 1 domain active */
        if (virt_info.num_domains > 0)
        {
            /* Get host memory details */
            virt_info.host_free_mem = (virNodeGetFreeMemory(virt_info.conn) / MEM_COORD_KB_SIZE);

            /* Check if error occurred */
            if (virt_info.host_free_mem == 0)
            {
                /* Set host free memory error */
                status = MEM_COORD_HOST_FREE_MEM_ERROR;
            }
            else
            {
                /* Get the host info to get memory available on the host */
                status = virNodeGetInfo(virt_info.conn, &info);

                /* Ensure host info obtained successfully */
                if (status == EXIT_SUCCESS)
                {
                    /* Set total memory available on host */
                    virt_info.host_total_mem = info.memory;

                    /* Calculate target memory size for host */
                    virt_info.host_tgt_mem = (MEM_COORD_AVAIL_HOST_TGT_PERCENT * virt_info.host_total_mem)/100;

                    /* Initialize the memory info structures */
                    status = vm_mem_info_init();
                }
                else
                {
                    /* Set error code */
                    status = MEM_COORD_DOMAIN_MEM_ERROR;
                }
            }
        }
        else
        {
            /* Check if error returned when trying to get list of domains */
            if (virt_info.num_domains < 0)
            {
                /* Set error status appropriately */
                status = MEM_COORD_DOMAIN_LIST_ERROR;
            }
            else
            {
                /* Set error for no domains */
                status = MEM_COORD_NO_DOMAINS;
            }
        }
    }
    else
    {
        /* Return error */
        status = MEM_COORD_CONN_ERROR;
    }

    /* Return status to caller */
    return (status);
}


/*************************************************************************
*
*   FUNCTION
*
*       vm_mem_info_init
*
*   DESCRIPTION
*
*       Initialize the VM Memory Info data structures
*
*   INPUTS
*
*       None
*
*   OUTPUTS
*
*       EXIT_SUCCESS                        VM Memory Info data structures initialized
*       Other                               Error with memory allocation or
*                                           using libvirt
*
*************************************************************************/
static int vm_mem_info_init(void)
{
    int     index, status = EXIT_SUCCESS;


    /* Allocate memory for VM memory information structure for each VM */
    vm_mem_info = calloc(virt_info.num_domains * sizeof(VM_MEM_INFO), 1);

    /* Ensure memory allocated */
    if (vm_mem_info != NULL)
    {
        /* Loop through each VM */
        for (index = 0; (index < virt_info.num_domains) && (status == EXIT_SUCCESS) ; index++)
        {
            /* Initialize the rate at which the balloon driver stats get updated to every 1 sec */
            status = virDomainSetMemoryStatsPeriod (virt_info.domain_list[index], 1, VIR_DOMAIN_AFFECT_LIVE);

            /* Ensure memory stats period successfully updated */
            if (status == EXIT_SUCCESS)
            {
                /* Get maximum memory set for this VM */
                vm_mem_info[index].mem_max = virDomainGetMaxMemory(virt_info.domain_list[index]);

                /* Check if max VM memory not obtained */
                if (vm_mem_info[index].mem_max == 0)
                {
                    /* Set status to error */
                    status = MEM_COORD_DOMAIN_MEM_ERROR;
                }
            }
        }
    }
    else
    {
        /* Set error status */
        status = MEM_COORD_NOMEM;
    }

    /* Return status to caller */
    return (status);
}


/*************************************************************************
*
*   FUNCTION
*
*       virt_deinit
*
*   DESCRIPTION
*
*       De-initializes all the virtualization content - frees data structures,
*       closes connections, etc.
*
*   INPUTS
*
*       None
*
*   OUTPUTS
*
*       None
*
*************************************************************************/
static void virt_deinit(void)
{
    /* Loop through and free all domain names */
    do
    {
        /* Decrement number of domains to process */
        virt_info.num_domains--;

        /* Free returned names */
        virDomainFree(virt_info.domain_list[virt_info.num_domains]);

    /* Keep looping until all domain names are free */
    } while (virt_info.num_domains);

    /* Free the list */
    free(virt_info.domain_list);

    /* Close connection to hypervisor */
    virConnectClose(virt_info.conn);
}


#if (MEM_COORD_DEBUG == 1)
/*************************************************************************
*
*   FUNCTION
*
*       dump_mem_stats
*
*   DESCRIPTION
*
*       Dump memory status to stdout
*
*   INPUTS
*
*       None
*
*   OUTPUTS
*
*       None
*
*************************************************************************/
static void dump_mem_stats(void)
{
    int     index;


    /* Output header */
    printf("\nMemory Stats\n");
    printf("============\n");

    /* Output host free memory */
    printf("Host Free Memory = %lld MBytes\n\n", (virt_info.host_free_mem / MEM_COORD_KB_SIZE));

    /* Loop through each VM */
    for (index = 0; index < virt_info.num_domains ; index++)
    {
        /* Print VM name */
        printf("VM name          = %s\n", virDomainGetName(virt_info.domain_list[index]));

        /* Output VM balloon memory info */
        printf("    Balloon Size = %lld MBytes\n", (vm_mem_info[index].mem_total / MEM_COORD_KB_SIZE));

        /* Output VM available memory info */
        printf("    Avail Size   = %lld MBytes\n", (vm_mem_info[index].mem_free / MEM_COORD_KB_SIZE));

        /* Print % available memory for the VM */
        printf("    Percent Avail= %d\n\n", vm_mem_info[index].percent_avail);
    }
}
#endif  /* (MEM_COORD_DEBUG == 1) */
