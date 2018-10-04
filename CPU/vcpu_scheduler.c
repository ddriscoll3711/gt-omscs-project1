/************************************************************************
*
*   DESCRIPTION
*
*       This file contains VCPU scheduler code that analyzes CPU
*       usage of 1 to many virtual machines and adjusts CPU assignment
*       for each virtual machine based on implemented algorithm.
*
*   FUNCTIONS
*
*       main
*       vcpu_schedule
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
#include "vcpu_scheduler_defs.h"

/*****************************/
/* LOCAL FUNCTION PROTOTYPES */
/*****************************/
static int  scheduler(unsigned int cycle_time);
static int  collect_pcpu_stats(unsigned long long ns_cycle_time);
static int  collect_vcpu_stats(unsigned long long ns_cycle_time);
static int  virt_init(void);
static int  pcpu_stats_init(void);
static int  vcpu_stats_init(void);
static void vcpu_unpin_from_pcpu(VCPU_STATS * vcpu, PCPU_STATS * pcpu);
static int  vcpu_pin_on_pcpu(VCPU_STATS * vcpu, PCPU_STATS * pcpu);
static int  vcpu_pinning_adjust(void);
static void virt_deinit(void);
static int  pcpu_get_idle(int num_params, unsigned long long * idle_time);
#if (VCPU_SCHEDULER_DEBUG == 1)
static void dump_scheduler_stats(void);
#endif  /* (VCPU_SCHEDULER_DEBUG == 1) */

/*****************************/
/* LOCAL VARIABLES           */
/*****************************/
static VIRT_INFO            virt_info;
static PCPU_STATS *         pcpu_stats;
static VCPU_STATS *         vcpu_stats;


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
            status = scheduler(seconds);

            /* Deinit the virtualization data */
            virt_deinit();
        }
    }

    /* Return status to caller */
    return (status);
}


/*************************************************************************
*
*   FUNCTION
*
*       scheduler
*
*   DESCRIPTION
*
*       Coordinates VCPU assignment / scheduling amongst VMs in the system
*       by pinning VCPUs to physical CPUs (PCPUs) based on CPU utilization.
*
*       Goal is to maximize all VMs performance by giving more dedicated
*       PCPU time to VMs that require it.
*
*   INPUTS
*
*       cycle_time                          Time, in seconds, to run
*                                           each cycle of the VCPU scheduler
*
*   OUTPUTS
*
*       EXIT_SUCCESS                        VCPU scheduling successful
*       Others                              Error during VCPU scheduling
*
*************************************************************************/
static int  scheduler(unsigned int cycle_time)
{
    int                 status = EXIT_SUCCESS;
    unsigned long long  ns_cycle_time = cycle_time * VCPU_SCHEDULER_SEC_TO_NANOSECS;


    /* Loop while no errors and no key hit */
    while (status == EXIT_SUCCESS)
    {
        /* Sleep for specified period of time */
        sleep(cycle_time);

        /* Collect PCPU stats */
        status = collect_pcpu_stats(ns_cycle_time);

        /* Ensure PCPU stats obtained successfully */
        if (status == EXIT_SUCCESS)
        {
            /* Collect VCPU stats for all domains */
            status = collect_vcpu_stats(ns_cycle_time);
        }

        /* Ensure VCPU stats obtained successfully */
        if (status == EXIT_SUCCESS)
        {
            /* Adjust pinning of VCPUs to PCPUs based
               on latest stats */
            status = vcpu_pinning_adjust();
        }

#if (VCPU_SCHEDULER_DEBUG == 1)
        /* Dump stats */
        dump_scheduler_stats();
#endif  /* (VCPU_SCHEDULER_DEBUG == 1) */
    }

    /* Return status to caller */
    return (status);
}


/*************************************************************************
*
*   FUNCTION
*
*       collect_pcpu_stats
*
*   DESCRIPTION
*
*       Collects PCPU stats
*
*   INPUTS
*
*       ns_cycle_time                       Number of nanoseconds per cycle
*
*   OUTPUTS
*
*       EXIT_SUCCESS                        PCPU stats collected
*       Others                              Error trying to collect PCPU stats
*
*************************************************************************/
static int  collect_pcpu_stats(unsigned long long ns_cycle_time)
{
    int                 index, status = EXIT_SUCCESS;
    unsigned long long  pcpu_idle;


    /* Reset low / high PCPU utilization masks before each new collection */
    virt_info.pcpu_high_mask = 0;
    virt_info.pcpu_low_mask = 0;

    /* Loop through each PCPU */
    for (index = 0; (index < virt_info.num_pcpus) && (status == EXIT_SUCCESS) ; index++)
    {
        /* Get this PCPUs information */
        status = virNodeGetCPUStats(virt_info.conn, index, virt_info.params, &virt_info.num_params, 0);

        /* See if PCPU stats obtained */
        if (status == EXIT_SUCCESS)
        {
            /* Try to get PCPU idle time */
            status = pcpu_get_idle(virt_info.num_params, &pcpu_idle);

            /* Ensure PCPU idle obtained */
            if (status == EXIT_SUCCESS)
            {
                /* Calculate PCPU utilization for last cycle (100 - idle time = CPU utilization) */
                pcpu_stats[index].cpu_util = (100 - ((pcpu_idle - pcpu_stats[index].last_time) * 100) / ns_cycle_time);

                /* Update last idle time */
                pcpu_stats[index].last_time = pcpu_idle;

                /* Check if PCPU utilization is above configured high threshold */
                if (pcpu_stats[index].cpu_util > VCPU_SCHEDULER_PCPU_HIGH_THRESHOLD)
                {
                    /* Check if this PCPU has more than 1 VCPU pinned to it */
                    if (pcpu_stats[index].num_pinned > 1)
                    {
                        /* Set bit in bitmask tracking this as candidate for
                           repinning of VCPUs */
                        virt_info.pcpu_high_mask |= (1 << index);
                    }
                }
                else
                {
                    /* Check if PCPU utilization is below configured low threshold */
                    if (pcpu_stats[index].cpu_util < VCPU_SCHEDULER_PCPU_LOW_THRESHOLD)
                    {
                        /* Set bit identifying this as a low CPU utilization PCPU */
                        virt_info.pcpu_low_mask |= (1 << index);
                    }
                }
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
*       collect_vcpu_stats
*
*   DESCRIPTION
*
*       Collects VCPU stats for each active VM
*
*   INPUTS
*
*       ns_cycle_time                       Number of nanoseconds per cycle
*
*   OUTPUTS
*
*       EXIT_SUCCESS                        VCPU stats for each VM collected
*       Others                              Error trying to collect VCPU stats
*
*************************************************************************/
static int  collect_vcpu_stats(unsigned long long ns_cycle_time)
{
    int                 index, status = EXIT_SUCCESS;
    virVcpuInfo         info;


    /* Loop through each VCPU */
    for (index = 0; (index < virt_info.num_domains) && (status == EXIT_SUCCESS) ; index++)
    {
        /* Get this VCPUs information */
        status = virDomainGetVcpus(vcpu_stats[index].domain_id, &info, 1, NULL, 0);

        /* Check if VCPU info obtained */
        if (status > 0)
        {
            /* Calculate VCPU utilization for last cycle */
            vcpu_stats[index].cpu_util = (int)(((info.cpuTime - vcpu_stats[index].last_time) * 100)/(ns_cycle_time));

            /* Save this cycle's CPU time */
            vcpu_stats[index].last_time = info.cpuTime;

            /* Successful VCPU info */
            status = EXIT_SUCCESS;
        }
        else
        {
            /* Consider no info or error from API a problem */
            status = VCPU_SCHEDULER_DOMAIN_INFO_ERROR;
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
    int     status = EXIT_SUCCESS;


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
            /* Get number of physical CPUs available on host */
            virt_info.num_pcpus = virNodeGetCPUMap(virt_info.conn, NULL, NULL, 0);

            /* Determine size of params for PCPU params
               NOTE:  This code assumes all PCPU's will have same size params */
            status = virNodeGetCPUStats(virt_info.conn, 0, NULL, &virt_info.num_params, 0);

            /* Ensure CPU stats returned successfully */
            if (status == EXIT_SUCCESS)
            {
                /* Allocate memory for params for a PCPU */
                virt_info.params = calloc(virt_info.num_params * sizeof(virNodeCPUStats), 1);

                /* Ensure param memory allocated */
                if (virt_info.params != NULL)
                {
                    /* Initialize PCPUs */
                    status = pcpu_stats_init();

                    /* Check if size of params determined */
                    if (status == EXIT_SUCCESS)
                    {
                        /* Initialize VCPUs */
                        status = vcpu_stats_init();
                    }
                }
                else
                {
                    /* Set error status showing no memory available */
                    status = VCPU_SCHEDULER_NOMEM;
                }
            }
        }
        else
        {
            /* Check if error returned when trying to get list of domains */
            if (virt_info.num_domains < 0)
            {
                /* Set error status appropriately */
                status = VCPU_SCHEDULER_DOMAIN_LIST_ERROR;
            }
            else
            {
                /* Set error for no domains */
                status = VCPU_SCHEDULER_NO_DOMAINS;
            }
        }
    }
    else
    {
        /* Return error */
        status = VCPU_SCHEDULER_CONN_ERROR;
    }

    /* Return status to caller */
    return (status);
}


/*************************************************************************
*
*   FUNCTION
*
*       pcpu_stats_init
*
*   DESCRIPTION
*
*       Initialize the PCPU stats data structures
*
*   INPUTS
*
*       None
*
*   OUTPUTS
*
*       EXIT_SUCCESS                        PCPU data structures initialized
*       Other                               Error with memory allocation or
*                                           using libvirt
*
*************************************************************************/
static int pcpu_stats_init(void)
{
    int index, status = EXIT_SUCCESS;


    /* Allocate memory for PCPU structures */
    pcpu_stats = calloc(virt_info.num_pcpus * sizeof(PCPU_STATS), 1);

    /* Ensure memory allocated */
    if (pcpu_stats != NULL)
    {
        /* Loop through and initialize each PCPU status structure */
        for (index = 0 ; (index < virt_info.num_pcpus) && (status == EXIT_SUCCESS) ; index++)
        {
            /* Set ID and cpu mask for each PCPU */
            pcpu_stats[index].id = index;
            pcpu_stats[index].cpumap = (1 << index);

            /* Get this PCPUs initial information */
            status = virNodeGetCPUStats(virt_info.conn, index, virt_info.params, &virt_info.num_params, 0);

            /* Ensure success here */
            if (status == EXIT_SUCCESS)
            {
                /* Get PCPU idle time */
                status = pcpu_get_idle(virt_info.num_params, &(pcpu_stats[index].last_time));
            }
        }   /* for loop */
    }
    else
    {
        /* Set error status showing no memory available */
        status = VCPU_SCHEDULER_NOMEM;
    }

    /* Return status to caller */
    return (status);
}


/*************************************************************************
*
*   FUNCTION
*
*       vcpu_stats_init
*
*   DESCRIPTION
*
*       Initialize the VCPU stats data structures
*
*   INPUTS
*
*       None
*
*   OUTPUTS
*
*       EXIT_SUCCESS                        VCPU data structures initialized
*       Other                               Error with memory allocation or
*                                           using libvirt
*
*************************************************************************/
static int vcpu_stats_init(void)
{
    int             index, pcpu_index, status = EXIT_SUCCESS;
    virVcpuInfo     info;


    /* Allocate memory for each of the VCPU stats structure (1 per domain) */
    vcpu_stats = calloc(virt_info.num_domains * sizeof(VCPU_STATS), 1);

    /* Ensure memory allocated */
    if (vcpu_stats != NULL)
    {
        /* Loop through and initialize each VCPU status structure */
        for (index = 0 ; (index < virt_info.num_domains) && (status == EXIT_SUCCESS); index++)
        {
            /* Assign domain ID for VCPU */
            vcpu_stats[index].domain_id = virt_info.domain_list[index];

            /* Determine PCPU index to pin this VCPU - initially just balance all VCPUs
               among available PCPUs */
            pcpu_index = index % virt_info.num_pcpus;

            /* Pin VCPU to the appropriate PCPU */
            status = vcpu_pin_on_pcpu(&vcpu_stats[index], &pcpu_stats[pcpu_index]);

            /* Make sure VCPU pinning worked */
            if (status == EXIT_SUCCESS)
            {
                /* Get this VCPUs initial status */
                status = virDomainGetVcpus(vcpu_stats[index].domain_id, &info, 1, NULL, 0);

                /* Check if initial status obtained */
                if (status > 0)
                {
                    /* Initialize VCPU stats with this information */
                    vcpu_stats[index].last_time = info.cpuTime;

                    /* Set success */
                    status = EXIT_SUCCESS;
                }
                else
                {
                    /* Set domain info error */
                    status = VCPU_SCHEDULER_DOMAIN_INFO_ERROR;
                }
            }
        }   /* for loop */
    }
    else
    {
        /* Set error status */
        status = VCPU_SCHEDULER_NOMEM;
    }

    /* Return status to caller */
    return (status);
}


/*************************************************************************
*
*   FUNCTION
*
*       vcpu_pinning_adjust
*
*   DESCRIPTION
*
*       Adjusts / changes VCPU to PCPU pinning based on latest stats
*       taken for VCPU and PCPU CPU utilization and specified
*       thresholds for changing pinning
*
*   INPUTS
*
*       None
*
*   OUTPUTS
*
*       EXIT_SUCCESS                        Adjustment of pinning for VCPU
*                                           was successful
*       Other                               Error when pinning of VCPU
*
*************************************************************************/
static int  vcpu_pinning_adjust(void)
{
    int             pcpu_high, pcpu_low, status = EXIT_SUCCESS;
    int             vcpu_delta, vcpu_best_delta, new_pcpu_util;
    unsigned int    pcpu_high_mask, pcpu_low_mask;
    VCPU_STATS *    vcpu;
    VCPU_STATS *    best_vcpu;


    /* Make a local copy of the low PCPU utilization mask */
    pcpu_low_mask = virt_info.pcpu_low_mask;

    /* Loop until all low / high utilized PCPUs are adjusted and no errors */
    while ((pcpu_low_mask) && (virt_info.pcpu_high_mask) && (status == EXIT_SUCCESS))
    {
        /* Get index of low loaded PCPU */
        LOW_BIT_GET32(pcpu_low_mask, &pcpu_low);

        /* Make a local copy of the high PCPU Utilization mask */
        pcpu_high_mask = virt_info.pcpu_high_mask;

        /* Reset best VCPU delta for best fit */
        vcpu_best_delta = 100;

        /* Set best VCPU pointer to NULL */
        best_vcpu = NULL;

        /* Loop until all high utilized PCPUs are processed for a best
           fit on the low utilized PCPUs */
        while ((pcpu_high_mask) && (status == EXIT_SUCCESS))
        {
            /* Get index of high loaded PCPU */
            LOW_BIT_GET32(pcpu_high_mask, &pcpu_high);

            /* Get head of VCPU list from highly loaded PCPU */
            vcpu = pcpu_stats[pcpu_high].head;

            /* Find best fit VCPU to move to less loaded PCPU */
            do
            {
                /* Calculate what PCPU utilization this will make for the currently low PCPU */
                new_pcpu_util = (vcpu->cpu_util + pcpu_stats[pcpu_low].cpu_util);

                /* Calculate how close to target utilization repinning this VCPU will come */
                vcpu_delta = abs(VCPU_SCHEDULER_PCPU_TGT - new_pcpu_util);

                /* Check to see if this VCPU is best fit AND migration doesn't cause similar high PCPU load */
                if ((vcpu_delta < vcpu_best_delta) && (new_pcpu_util < VCPU_SCHEDULER_PCPU_HIGH_THRESHOLD))
                {
                    /* Set new best delta */
                    vcpu_best_delta = vcpu_delta;

                    /* Save pointer to this VCPU */
                    best_vcpu = vcpu;
                }

                /* Move to next VCPU in list */
                vcpu = vcpu->next;

            /* Loop until back to start of VCPU list */
            } while (vcpu != pcpu_stats[pcpu_high].head);

            /* Clear PCPU high mask bit for this PCPU */
            pcpu_high_mask &= ~(1 << pcpu_high);
        }

        /* Clear PCPU low mask bit since we have looked through
           every option to move a VCPU from all highly loaded PCPUs
           to this low loaded PCPU */
        pcpu_low_mask &=  ~(1 << pcpu_low);

        /* Ensure the best VCPU was found */
        if (best_vcpu != NULL)
        {
            /* Clear high PCPU mask bit for the VCPU being migrated */
            virt_info.pcpu_high_mask &= ~(1 << best_vcpu->pcpu->id);

            /* Move best fit VCPU from current PCPU to less loaded PCPU */
            status = vcpu_pin_on_pcpu(best_vcpu, &pcpu_stats[pcpu_low]);
        }

    }   /* while loop */

    /* Return status to caller */
    return (status);
}


/*************************************************************************
*
*   FUNCTION
*
*       vcpu_unpin_from_pcpu
*
*   DESCRIPTION
*
*       Unpins specified VCPU from PCPU and removes from PCPU list
*
*   INPUTS
*
*       vcpu                                Pointer to VCPU to remove from list
*       pcpu                                Pointer to PCPU which contains VCPU
*
*   OUTPUTS
*
*       None
*
*************************************************************************/
static void     vcpu_unpin_from_pcpu(VCPU_STATS * vcpu, PCPU_STATS * pcpu)
{
    /* Ensure VCPU is currently on this PCPU list */
    if ((vcpu->pcpu != NULL) && (vcpu->pcpu == pcpu))
    {
        /* Decrement number of VCPUs pinned to this PCPU */
        pcpu->num_pinned--;

        /* Check if only node on list */
        if (vcpu->prev == vcpu)
        {
            /* Set head pointer to NULL */
            pcpu->head = NULL;
        }
        else
        {
            /* Adjust prev / next pointers of adjacent nodes in list */
            vcpu->prev->next = vcpu->next;
            vcpu->next->prev = vcpu->prev;

            /* Check if this VCPU was at head of list */
            if (pcpu->head == vcpu)
            {
                /* Set new head */
                pcpu->head = vcpu->next;
            }
        }

        /* Set VCPU next / previous to NULL */
        vcpu->next = NULL;
        vcpu->prev = NULL;
    }
}


/*************************************************************************
*
*   FUNCTION
*
*       vcpu_pin_on_pcpu
*
*   DESCRIPTION
*
*       Pins the specified VCPU to the specified PCPU and keeps
*       track of this association via a linked list
*
*   INPUTS
*
*       vcpu                                Pointer to VCPU to pin
*       pcpu                                Pointer on which VCPU is pinned
*
*   OUTPUTS
*
*       None
*
*************************************************************************/
static int  vcpu_pin_on_pcpu(VCPU_STATS * vcpu, PCPU_STATS * pcpu)
{
    int     status;


    /* Pin this VCPU the specified PCPU
       NOTE:  This implementation only supports configurations with single
              VCPU per Domain/VM and a maximum of 8 PCPUs */
    status = virDomainPinVcpu(vcpu->domain_id, 0, &(pcpu->cpumap), 1);

    /* Ensure VCPU successfully pinned to PCPU */
    if (status == EXIT_SUCCESS)
    {
        /* Ensure this VCPU is removed from it's current PCPU list */
        vcpu_unpin_from_pcpu(vcpu, vcpu->pcpu);

        /* Point this VCPU structure to PCPU on which it is pinned */
        vcpu->pcpu = pcpu;

        /* Increment number of VCPUs pinned to this PCPU */
        pcpu->num_pinned++;

        /* Check if the PCPU list of VCPUs is empty */
        if (pcpu->head == NULL)
        {
            /* First VCPU on list - put at head of list and point to itself */
            pcpu->head          = vcpu;
            vcpu->next          = vcpu;
            vcpu->prev          = vcpu;
        }
        else
        {
            /* Not empty list - put this VCPU at end of the list */
            vcpu->prev          = pcpu->head->prev;
            vcpu->prev->next    = vcpu;
            vcpu->next          = pcpu->head;
            pcpu->head->prev    = vcpu;
        }
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
    /* Deallocate params memory */
    free(virt_info.params);

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


/*************************************************************************
*
*   FUNCTION
*
*       pcpu_get_idle
*
*   DESCRIPTION
*
*       Gets PCPU idle time from params
*
*   INPUTS
*
*       num_params                          Number of parameters
*
*   OUTPUTS
*
*       unsigned long long                  PCPU idle time
*
*************************************************************************/
static int  pcpu_get_idle(int num_params, unsigned long long * idle_time)
{
    int     status = VCPU_SCHEDULER_PCPU_IDLE_ERROR;


    /* Loop through all parameters */
    while (num_params)
    {
        /* Decrement number of parameters */
        num_params--;

        /* Check if this is the idle parameter */
        if (strcmp(virt_info.params[num_params].field, VIR_NODE_CPU_STATS_IDLE) == 0)
        {
            /* Set return value */
            *idle_time = virt_info.params[num_params].value;

            /* Set success status */
            status = EXIT_SUCCESS;

            /* Exit the loop */
            break;
        }
    }

    /* Return status to caller */
    return (status);
}

#if (VCPU_SCHEDULER_DEBUG == 1)
/*************************************************************************
*
*   FUNCTION
*
*       dump_scheduler_stats
*
*   DESCRIPTION
*
*       Dumps VCPU stats to standard out
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
static void dump_scheduler_stats(void)
{
    int     index;


    /* Output header */
    printf("\nPCPU Stats\n");
    printf("==========\n");

    /* Loop through each PCPUs */
    for (index = 0; index < virt_info.num_pcpus ; index++)
    {
        /* Output PCPU utilization time for this cycle */
        printf("PCPU = %d\n", index);
        printf("    CPU Util = %d\n", pcpu_stats[index].cpu_util);
    }

    /* Output header */
    printf("\nVCPU Stats\n");
    printf("==========\n");

    /* Loop through each VCPU */
    for (index = 0; index < virt_info.num_domains ; index++)
    {
        /* Output info for PCPU */
        printf("VM name       = %s\n", virDomainGetName(vcpu_stats[index].domain_id));

        /* Output PCPU pinning info */
        printf("    PCPU Pin = %d\n", vcpu_stats[index].pcpu->id);

        /* Output CPU utilization over entire test */
        printf("    CPU Util = %d\n", vcpu_stats[index].cpu_util);
    }
}
#endif  /* (VCPU_SCHEDULER_DEBUG == 1) */
