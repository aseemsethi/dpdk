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

#include <rte_common.h>
#include <rte_memory.h>
#include <rte_malloc.h>   // rte_free
#include <rte_errno.h>    // rte_strerror
#include <rte_string_fns.h>
#include <rte_version.h>
#include <rte_atomic.h>
#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_kni.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
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

/* Max size of a single packet */
#define MAX_PACKET_SZ           2048

/* Size of the data buffer in each mbuf */
#define MBUF_DATA_SZ (MAX_PACKET_SZ + RTE_PKTMBUF_HEADROOM)

/* Number of mbufs in mempool that is created */
#define NB_MBUF                 100
//(8192 * 16)

/* How many packets to attempt to read from NIC in one go */
#define PKT_BURST_SZ            32

/* How many objects (mbufs) to keep in per-lcore mempool cache */
#define MEMPOOL_CACHE_SZ        PKT_BURST_SZ

/* Number of RX ring descriptors */
#define NB_RXD                  1024

/* Number of TX ring descriptors */
#define NB_TXD                  1024

/* Total octets in ethernet header */
#define KNI_ENET_HEADER_SIZE    14

/* Total octets in the FCS */
#define KNI_ENET_FCS_SIZE       4

#define KNI_US_PER_SECOND       1000000
#define KNI_SECOND_PER_DAY      86400

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
/* Mempool for mbufs */
static struct rte_mempool * pktmbuf_pool = NULL;
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

static void printConfig(void) {
    uint32_t i, j;
    struct kni_port_params **p = kni_port_params_array;

    for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
        if (!p[i])
            continue;
        RTE_LOG(INFO, APP, "Port ID: %d\n", p[i]->port_id);
        RTE_LOG(INFO, APP, "Rx lcore ID: %u, Tx lcore ID: %u\n",
                    p[i]->lcore_rx, p[i]->lcore_tx);
        for (j = 0; j < p[i]->nb_lcore_k; j++)
            RTE_LOG(INFO, APP, "Kernel thread lcore ID: %u\n",
                            p[i]->lcore_k[j]);
    }
}

/*
To run the application with two ports served by six lcores, one lcore of RX, 
one lcore of TX, and one lcore of kernel thread for each port:
./build/kni -l 4-7 -n 4 -- -P -p 0x3 --config="(0,4,6,8),(1,5,7,9)"
–config=”(port,lcore_rx, lcore_tx[,lcore_kthread, ...]) 
			[, port,lcore_rx, lcore_tx[,lcore_kthread, ...]]”
	- Determines which lcores of RX, TX, kernel thread are mapped to which ports.

#insmod rte_kni.ko kthread_mode=single
This mode will create only one kernel thread for all KNI devices for packet receiving 
in kernel side. By default, it is in this single kernel thread mode. It can set 
core affinity for this kernel thread by using Linux command taskset.
#taskset -p 100000 `pgrep --fl kni_thread | awk '{print $1}'`

*/
static int parseConfig(const char *optarg) {
	const char *p, *p0 = optarg;
	char s[256], *end;
	unsigned size, i, j, nb_token;
	uint16_t nb_kni_port_params = 0;
    enum fieldnames {
        FLD_PORT = 0,
        FLD_LCORE_RX,
        FLD_LCORE_TX,
        _NUM_FLD = KNI_MAX_KTHREAD + 3,
    };
    char *str_fld[_NUM_FLD];
    unsigned long int_fld[_NUM_FLD];
	uint16_t port_id;

	printf("MyApp: RTE_MAX_ETHPORTS=%d, KNI_MAX_KTHREAD=%d\n",
			RTE_MAX_ETHPORTS, KNI_MAX_KTHREAD);
	memset(&kni_port_params_array, 0, sizeof(kni_port_params_array));
	while(((p = strchr(p0, '(')) != NULL) &&
		nb_kni_port_params < RTE_MAX_ETHPORTS) {
		p++;
		if ((p0 = strchr(p, ')')) == NULL)
			goto fail;
		size = p0 - p;
		if(size > sizeof(s)) {
			printf("MyApp: Invalid config params\n");
			goto fail;
		}
		// asterisk (*) is used to pass the width specifier/precision
		snprintf(s, sizeof(s), "%.*s", size, p);
		nb_token = rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',');
		printf("MyApp: config option:%.*s, nb_token=%d\n", size, p, nb_token);
		if (nb_token <= FLD_LCORE_TX) {
			printf("MyApp: Invalid config params\n");
			goto fail;
		}
		for(i = 0; i <nb_token; i++) {
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i]) {
			}
		}
		i = 0;
		port_id = int_fld[i++];
		printf("MyApp: port id: %d\n", port_id);
		if(kni_port_params_array[port_id]) {
			printf("MyApp: Port %d has already been configured\n", port_id);
			goto fail;
		}
		kni_port_params_array[port_id] = rte_zmalloc("KNI_port_params",
				sizeof(struct kni_port_params), RTE_CACHE_LINE_SIZE);
		        kni_port_params_array[port_id]->port_id = port_id;
        kni_port_params_array[port_id]->lcore_rx = (uint8_t)int_fld[i++];
        kni_port_params_array[port_id]->lcore_tx = (uint8_t)int_fld[i++];
		if (kni_port_params_array[port_id]->lcore_rx > RTE_MAX_LCORE ||
			kni_port_params_array[port_id]->lcore_tx >= RTE_MAX_LCORE) {
			printf("lcore_rx %u or lcore_tx %u ID could not "
                        "exceed the maximum %u\n",
			kni_port_params_array[port_id]->lcore_rx,
			kni_port_params_array[port_id]->lcore_tx, (unsigned)RTE_MAX_LCORE);
            goto fail;
        }
		for (j=0; i< nb_token && j < KNI_MAX_KTHREAD; i++,j++)
			kni_port_params_array[port_id]->lcore_k[j] = (uint8_t)int_fld[i];
        kni_port_params_array[port_id]->nb_lcore_k = j;
	}
	printConfig();
	return 0;
fail:
	for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
		if (kni_port_params_array[i]) {
			rte_free(kni_port_params_array[i]);
			kni_port_params_array[i] = NULL;
		}
	}
	return 1;
}

/*
 * Display usage instructions
 */
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
	while((opt = getopt_long(argc, argv, "p:P", longopts, &longindex)) != EOF) {
	switch(opt) {
	case 'p':
		printf("MyApp: option p\n");
		break;
	case 'P':
		printf("MyApp: option P\n");
		promiscuous_on = 1;
		break;
	case 0:
            if (!strncmp(longopts[longindex].name, "config", sizeof("config"))) {
				printf("MyApp: option config found\n");
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
	printf("MyApp: rte_socket_id: %d\n", rte_socket_id());
	// Create the mbuf pool
	pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", NB_MBUF, 
				MEMPOOL_CACHE_SZ, 0, MBUF_DATA_SZ, rte_socket_id());
	if(pktmbuf_pool == NULL) {
		rte_exit(EXIT_FAILURE, " mbuf pool failure: Error Code: %d:%s\n",	
			rte_errno, rte_strerror(rte_errno));
		return -1;
	}

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

