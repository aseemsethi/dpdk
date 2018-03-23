/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/queue.h>
#include <signal.h>
#include <signal.h>
#include <getopt.h>


#include <rte_version.h>
#include <rte_atomic.h>
#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_kni.h>
#include <rte_log.h>
#include <rte_debug.h>

/*
./rte-app [-c COREMASK | -l CORELIST] [-n NUM] [-b <domain:bus:devid.func>] \
          [--socket-mem=MB,...] [-d LIB.so|DIR] [-m MB] [-r NUM] [-v] [--file-prefix] \
          [--proc-type <primary|secondary|auto>]
option c or l is mandatory
*/

// rte_kni.ko is present at 
// ./x86_64-native-linuxapp-gcc/build/lib/librte_eal/linuxapp/kni/rte_kni.ko

/* Macros for printing using RTE_LOG */
#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER1

#define KNI_MAX_KTHREAD 32

static rte_atomic32_t kni_stop = RTE_ATOMIC32_INIT(0);
struct kni_interface_stats {
	uint64_t	rx_pkts;
	uint64_t	rx_drops;
	uint64_t	tx_pkts;
	uint64_t	tx_drops;
};
static struct kni_interface_stats kni_stats[RTE_MAX_ETHPORTS];

struct kni_port_params {
	uint16_t port_id;
    unsigned lcore_rx; /* lcore ID for RX */
    unsigned lcore_tx; /* lcore ID for TX */
    uint32_t nb_lcore_k; /* Number of lcores for KNI multi kernel threads */
    uint32_t nb_kni; /* Number of KNI devices to be created */
    unsigned lcore_k[KNI_MAX_KTHREAD]; /* lcore ID list for kthreads */
    struct rte_kni *kni[KNI_MAX_KTHREAD]; /* KNI context pointers */
} __rte_cache_aligned;

static struct kni_port_params *kni_port_params_array[RTE_MAX_ETHPORTS];
static int promiscuous_on = 0;

static int lcore_hello(__attribute__((unused)) void *arg) {
	unsigned lcore_id;
	lcore_id = rte_lcore_id();
	printf("MyApp: Hello from core %u\n", lcore_id);
	return 0;
}

static void printStats(void) {
	int i;
    printf("\n**KNI example application statistics**\n"
           "======  ==============  ============  ============  ============  ============\n"
           " Port    Lcore(RX/TX)    rx_packets    rx_dropped    tx_packets    tx_dropped\n"
           "------  --------------  ------------  ------------  ------------  ------------\n");
	for(i=0; i<RTE_MAX_ETHPORTS;i++) {
		if (!kni_port_params_array[i])
			continue;
		printf("%7d %10u/%2u %13"PRIu64" %13"PRIu64" %13"PRIu64" "
				"%13"PRIu64"\n", i,
                    kni_port_params_array[i]->lcore_rx,
                    kni_port_params_array[i]->lcore_tx,
                        kni_stats[i].rx_pkts,
                        kni_stats[i].rx_drops,
                        kni_stats[i].tx_pkts,
                        kni_stats[i].tx_drops);
    }
    printf("======  ==============  ============  ============  ============  ============\n");

}

static void signal_handler(int signum) {
	if(signum == SIGUSR1) {
		printf("MyApp: SIGUSR1 - Stats\n");
		printStats();
		return;
	}
	if(signum == SIGUSR2) {
		printf("MyApp: SIGUSR2 - Reset Stats\n");
		memset(&kni_stats, 0 , sizeof(kni_stats));
		return;
	}
	if(signum == SIGRTMIN || signum == SIGINT) {
		printf("MyApp: Stopping Processing...\n");
		rte_atomic32_inc(&kni_stop);
		return;
	}
}

static int parseConfig(const char *optarg) {
	printf("MyApp: Long option config, %d\n", *optarg);
	return 0;
}

/* Display usage instructions */
static void
printUsage(const char *prgname)
{
    RTE_LOG(INFO, APP, "\nUsage: %s [EAL options] -- -p PORTMASK -P "
           "[--config (port,lcore_rx,lcore_tx,lcore_kthread...)"
           "[,(port,lcore_rx,lcore_tx,lcore_kthread...)]]\n"
           "    -p PORTMASK: hex bitmask of ports to use\n"
           "    -P : enable promiscuous mode\n"
           "    --config (port,lcore_rx,lcore_tx,lcore_kthread...): "
           "port and lcore configurations\n",
               prgname);
}

static int parseArgs(int argc, char **argv) {
	int opt, longindex, ret = 0;
	const char* prgname = argv[0];
	static struct option longopts[] = {
		{"config", required_argument, NULL, 0},
		{NULL, 0, NULL, 0}
	};
	//opterr = 0;
	printf("MyApp: parsing args %d \n", argc);
	while((opt = getopt_long(argc, argv, "p:P", longopts, &longindex)) != EOF) {
	printf("MyApp: %d\n", opt);
	switch(opt) {
	case 'p':
		printf("MyApp: option p\n");
	case 'P':
		printf("MyApp: option P\n");
		promiscuous_on = 1;
	case 0:
            if (!strncmp(longopts[longindex].name, "config", sizeof("config"))) {
                ret = parseConfig(optarg);
                if (ret) {
                    printf("Invalid config\n");
                    printUsage(prgname);
                    return -1;
                }
            }
			break;
	default:
		printf("MyApp: parsing args err\n");
		printUsage(prgname);
		rte_exit(EXIT_FAILURE, "Invaid option specified\n");
	}
	}
	return 1;
}

int main(int argc, char** argv) {
	int ret;
	unsigned lcore_id;
    int32_t f_stop;

	ret = rte_eal_init(argc, argv);
	if(ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot init EAL:%d\n", ret);
	argc -= ret;
	argv += ret;
	printf("MyApp: Parsed %d args in eal_init, Remaing %d args\n", ret, argc-1);

	ret = parseArgs(argc, argv);
	if (ret < 0)	
		rte_exit(EXIT_FAILURE, "Could not parse input params\n");

	printf("Myapp: DPDK Version: %s\n", rte_version());
	// Associate signal handlers
	signal(SIGUSR1, signal_handler);
	signal(SIGUSR2, signal_handler);
	signal(SIGRTMIN, signal_handler);
	signal(SIGINT, signal_handler);

	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		printf("Myapp: Starting remote launch on lcore:%u\n", lcore_id);
		rte_eal_remote_launch(lcore_hello, NULL, lcore_id);
		// The MASTER lcore returns as soon as the message is sent and knows nothing 
		// about the completion of lcore_hello.
		// 2nd param is arg to be passed to the function
	}
	// Call it on the master core too - for a VM with just 1 core, only the following
	// call happens.
	lcore_hello(NULL);
	// Wait until all lcores finish their jobs.
	// To be executed on the MASTER lcore only. 
	// Issue an rte_eal_wait_lcore() for every lcore. The return values are ignored.
	// After a call to rte_eal_mp_wait_lcore(), the caller can assume that all slave 
	// lcores are in a WAIT state.
	rte_eal_mp_wait_lcore();

	// Instead of the above 2 functions, we could have called 1 function
	// rte_eal_mp_remote_launch(lcore_hello, NULL, CALL_MASTER);
	// that would launch the function on all cores



	// Run loop
	while(1) {
		f_stop = rte_atomic32_read(&kni_stop);
		if(f_stop)
			break;
	}
	return 0;
}

