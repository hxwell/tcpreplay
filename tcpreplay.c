/* $Id: tcpreplay.c,v 1.16 2002/07/10 05:35:53 aturner Exp $ */

#include "config.h"

#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <libnet.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "cache.h"
#include "cidr.h"
#include "libpcap.h"
#include "snoop.h"
#include "tcpreplay.h"

struct options options;
CACHE *cachedata = NULL;
CIDR *cidrdata = NULL;
struct timeval begin, end;
unsigned long bytes_sent, failed, pkts_sent;
int verbose, Rflag, Sflag, Cflag, uflag, cache_bit, cache_byte, cache_packets;
volatile int didsig;

#ifdef DEBUG
int debug = 0;
#endif

void replay_file(char *);
void do_packets(int, int (*)(int, struct packet *));
void do_sleep(struct timeval *, struct timeval *, int);
void catcher(int);
void packet_stats();
void usage();
void version();
void mac2hex(const char *, char *, int); 

int
main(int argc, char *argv[])
{
	char *cache_file = NULL, *intf = NULL, *intf2 = NULL, ebuf[256]; 
	int ch, i;

	bytes_sent = failed = pkts_sent = verbose = 0;
	intf = intf2 = NULL;
	memset(&options, 0, sizeof(options));

	/* Default mode is to replay pcap once at 10MB */
	options.mult = 0.0;
	options.n_iter = 1;
	options.rate = 10.0;

	Rflag =  Sflag = Cflag = uflag = 0;
	cache_bit = cache_byte = 0;

#ifdef DEBUG
	while ((ch = getopt(argc, argv, "dc:C:hi:I:j:J:l:m:r:RSu:Vv?")) != -1)
#else
	while ((ch = getopt(argc, argv, "c:C:hi:I:j:J:l:m:r:RSu:Vv?")) != -1)
#endif
		switch(ch) {
		case 'c': /* cache file */
			cache_file = optarg;
			cache_packets = read_cache(cache_file);
			break;
		case 'C': /* cidr matching */
			Cflag = 1;
			if (!parse_cidr(optarg))
				usage();
			break;
#ifdef DEBUG
		case 'd': /* enable debug */
			debug = 1;
			break;
#endif
		case 'i': /* interface */
			intf = optarg;
			break;
		case 'I': /* primary dest mac */
			mac2hex(optarg, options.intf1_mac, sizeof(options.intf1_mac));
			if (options.intf1_mac == NULL)
				errx(1, "Invalid mac address: %s", optarg);
			break;
		case 'j': /* secondary interface */
			intf2 = optarg;
			break;
		case 'J': /* secondary dest mac */
			mac2hex(optarg, options.intf2_mac, sizeof(options.intf2_mac));
			if (options.intf2_mac == NULL)
				errx(1, "Invalid mac address: %s", optarg);
			break;
		case 'l': /* loop count */
			options.n_iter = atoi(optarg);
			if (options.n_iter <= 0)
				errx(1, "Invalid loop count: %s", optarg);
			break;
		case 'm': /* multiplier */
			options.mult = atof(optarg);
			if (options.mult <= 0)
				errx(1, "Invalid multiplier: %s", optarg);
			options.rate = 0.0;
			break;
		case 'r': /* target rate */
			options.rate = atof(optarg);			
			if (options.rate <= 0)
				errx(1, "Invalid rate: %s", optarg);
			/* convert to bytes */
			options.rate = (options.rate * (1024*1024)) / 8;
			options.mult = 0.0;
			break;
		case 'R': /* replay at top speed */
			Rflag = 1;
			break;
		case 'S': /* snoop files */
			Sflag = 1;
			break;
		case 'v': /* verbose */
			verbose++;
			break;
		case 'u': /* untruncate packet */
			if (strcmp("pad", optarg) == 0) {
				uflag = PAD_PACKET;
			} else if (strcmp("trunc", optarg) == 0) {
				uflag = TRUNC_PACKET;
			} else {
				errx(1, "Invalid untruncate option: %s", optarg);
			}
			break;
		case 'V':
			version();
			break;
		default:
			usage();
		}

	argc -= optind;
	argv += optind;

	if ( (options.mult > 0.0 && options.rate > 0.0) || argc == 0)
		usage();

	if (argc > 1)
		for (i = 0; i < argc; i++)
			if (!strcmp("-", argv[i]))
				errx(1, "stdin must be the only file specified");

	if (intf == NULL)
		errx(1, "Must specify interface");

	if ((intf2 == NULL) && (cache_file != NULL))
		errx(1, "Needs secondary interface with cache");

	if ((intf2 != NULL) && (!Cflag && (cache_file == NULL) ))
		errx(1, "Needs cache or cidr match with secondary interface");

#if USE_LIBNET_VERSION == 10
	if ((options.intf1 = libnet_open_link_interface(intf, ebuf)) == NULL)
		errx(1, "Can't open %s: %s", intf, ebuf);

	if (intf2 != NULL && 
		(options.intf2 = libnet_open_link_interface(intf2, ebuf)) == NULL)
		errx(1, "Can't open %s: %s", intf2, ebuf);
#elif USE_LIBNET_VERSION == 11
	if ((options.intf1 = libnet_init(LIBNET_LINK_ADV, intf, ebuf)) == NULL)
		errx(1, "Can't open %s: %s", intf, ebuf);

	if ((options.intf2 = libnet_init(LIBNET_LINK_ADV, intf2, ebuf)) == NULL)
		errx(1, "Can't open %s: %s", intf2, ebuf);
#endif

	warnx("sending on %s %s", intf, intf2 == NULL ? "" : intf2);

	if (gettimeofday(&begin, NULL) < 0)
		err(1, "gettimeofday");

	/* main loop */
	while (options.n_iter--) {
		for (i = 0; i < argc; i++) {
			/* reset cache markers for each iteration */
			cache_byte = 0;
			cache_bit = 0;
			replay_file(argv[i]);
		}
	}

	if (bytes_sent > 0)
		packet_stats();

	return 0;
}

void
replay_file(char *path)
{
	int fd;

	if (!strcmp(path, "-")) {
		fd = STDIN_FILENO;
	} else if ((fd = open(path, O_RDONLY, 0)) < 0) {
		warn("skipping %s: could not open", path);
		return;
	}

	if (Sflag && is_snoop(fd)) {
		do_packets(fd, get_next_snoop);
		(void)close(fd);
	} else if (is_pcap(fd)) {
		do_packets(fd, get_next_pcap);
		(void)close(fd);
	} else {
		warnx("skipping %s: unknown format", path);
	}
}

void
do_packets(int fd, int (*get_next)(int, struct packet *))
{
	struct libnet_ethernet_hdr *eth_hdr = NULL;
#if USE_LIBNET_VERSION == 10
	struct libnet_link_int *l = NULL;
#elif USE_LIBNET_VERSION == 11
	libnet_t *l = NULL;
#endif
	ip_hdr_t *ip_hdr = NULL;
	struct packet pkt;
	struct timeval last;
	char *pktdata;
	int packet_num, ret, pktlen, proto;

	/* register signals */
	didsig = 0;
	(void)signal(SIGINT, catcher);

	packet_num = 0;
	timerclear(&last);

	while ( (*get_next) (fd, &pkt) ) {
		if (didsig) {
			packet_stats();
			_exit(1);
		}


		if (!Rflag)
			do_sleep(&pkt.ts, &last, pkt.len);

		eth_hdr = (struct libnet_ethernet_hdr *)(pkt.data);

		/* Dual nic processing */
		if (options.intf2 != NULL) {
			/* Cache Mode */
			if (!Cflag) {
				if (packet_num > cache_packets)
					errx(1, "Exceeded number of packets in cache file");

				if (cachedata->data[cache_byte] & 
					(char)pow((long)2, (long)cache_bit) ) {
					/* set interface to send out packet */
					l = options.intf1;

					/* check for destination MAC rewriting */
					if (options.intf1_mac != NULL) {
						memcpy(eth_hdr->ether_dhost, options.intf1_mac, ETHER_ADDR_LEN);
					}
				} else {
					/* set interface to send out packet */
					l = options.intf2;

					/* check for destination MAC rewriting */
					if (options.intf2_mac != NULL) {
						memcpy(eth_hdr->ether_dhost, options.intf2_mac, ETHER_ADDR_LEN);
					}
				} /* end cache processing */
			
				/* increment our bit/byte pointers for next time */
				if (cache_bit == 7) {
					cache_bit = 0;
					cache_byte++;
				} else {
					cache_bit++;
				}
			/* CIDR Mode */
			} else {
				ip_hdr = (ip_hdr_t *) (pkt.data + LIBNET_ETH_H);
				if (check_ip_CIDR(ip_hdr->ip_src.s_addr)) {
					/* set interface to send out packet */
					l = options.intf1;

					/* check for destination MAC rewriting */
					if (options.intf1_mac != NULL) {
						memcpy(eth_hdr->ether_dhost, options.intf1_mac, ETHER_ADDR_LEN);
					}
				} else {
					/* override interface to send out packet */
					l = options.intf2;

					/* check for destination MAC rewriting */
					if (options.intf2_mac != NULL) {
						memcpy(eth_hdr->ether_dhost, options.intf2_mac, ETHER_ADDR_LEN);
					}
				}
			}
		} else {
			/* normal operation */
			l = options.intf1;
			/* check for destination MAC rewriting */
			if (options.intf1_mac != NULL) {
				memcpy(eth_hdr->ether_dhost, options.intf1_mac, ETHER_ADDR_LEN);
			}
		}

		/* Untruncate packet? Only for IP packets */
		if (uflag && (pkt.len != pkt.actual_len) && 
			(ntohs(eth_hdr->ether_type) == ETHERTYPE_IP)) {
			/* Pad packet? */
			if (uflag == PAD_PACKET) {
				memset(pkt.data + pkt.len, 0, sizeof(pkt.data) - pkt.len);
				pkt.len = pkt.actual_len;
			} else { /* truncate packet */
				ip_hdr = (ip_hdr_t *)(pkt.data + LIBNET_ETH_H);
				ip_hdr->ip_len = htons(pkt.len);
			}
		
			/* recalc the checksum(s) */
			proto = ((ip_hdr_t *)(pkt.data + LIBNET_ETH_H))->ip_p;
#if USE_LIBNET_VERSION == 10
			if (libnet_do_checksum(pkt.data + LIBNET_ETH_H, proto, 
								   pkt.len - LIBNET_ETH_H - LIBNET_IP_H) < 0)
				warnx("Layer 4 checksum failed");

			if (libnet_do_checksum(pkt.data + LIBNET_ETH_H, IPPROTO_IP, 
								   LIBNET_IP_H) < 0)
				warnx("IP checksum failed");
#elif USE_LIBNET_VERSION == 11
			if (libnet_do_checksum(l, pkt.data + LIBNET_ETH_H, proto,
								   pkt.len - LIBNET_ETH_H - LIBNET_IP_H) < 0)
				warnx("Layer 4 checksum failed");

			if (libnet_do_checksum(l, pkt.data + LIBNET_ETH_H, proto,
								   pkt.len - LIBNET_ETH_H - LIBNET_IP_H) < 0)
				warnx("IP checksum failed");
#endif
								   
		} 
		pktdata = pkt.data;
		pktlen = pkt.len;

		/* Physically send the packet */
		do {
#if USE_LIBNET_VERSION == 10
			ret = libnet_write_link_layer(l, l->device, (u_char *)pktdata, pktlen);
#elif USE_LIBNET_VERSION == 11
			/*
			 * libnet_write_link() isn't part of the offical external API of Libnet 1.1
			 * so we're hoping that Mike S. doesn't change things.  If he does, we'll 
			 * have to figure out a work around.
			 */
			ret = libnet_write_link(l, (u_char*)pktdata, pktlen);
#endif
			if (ret == -1) {
				/* Make note of failed writes due to full buffers */
				if (errno == ENOBUFS) {
					failed++;
				} else {
#if USE_LIBNET_VERSION == 10
					err(1, "libnet_write_link_layer");
#elif USE_LIBNET_VERSION == 11
					err(1, "libnet_write_link");
#endif
				}
			}
		} while (ret == -1);

		bytes_sent += pkt.len;
		pkts_sent++;

		last = pkt.ts;
	}
}

/*
 * Given the timestamp on the current packet and the last packet sent,
 * calculate the appropriate amount of time to sleep and do so.
 */
void 
do_sleep(struct timeval *time, struct timeval *last, int len)
{
	static struct timeval didsleep;	
	static struct timeval start;	
	struct timeval nap, now, delta;
	float n;

	if (gettimeofday(&now, NULL) < 0)
		err(1, "gettimeofday");

	/* First time through for this file */
	if (!timerisset(last)) {
		start = now;
		timerclear(&delta);
		timerclear(&didsleep);
	} else {
		timersub(&now, &start, &delta);
	}

	if (options.mult) {
		/* 
		 * Replay packets a factor of the time they were originally sent.
		 */
		if (timerisset(last) && timercmp(time, last, >)) 
			timersub(time, last, &nap);
		else  
			/* 
			 * Don't sleep if this is our first packet, or if the
			 * this packet appears to have been sent before the 
			 * last packet.
			 */
			timerclear(&nap);

		timerdiv(&nap, options.mult);

	} else if (options.rate) {
		/* 
		 * Ignore the time supplied by the capture file and send data at
		 * a constant 'rate' (bytes per second).
		 */
		if (timerisset(last)) {
			n = (float)len / (float)options.rate;
			nap.tv_sec = n;
			nap.tv_usec = (n - nap.tv_sec) * 1000000;
		} else
			timerclear(&nap);
	}

	timeradd(&didsleep, &nap, &didsleep);

	if (timercmp(&didsleep, &delta, >)) {
		timersub(&didsleep, &delta, &nap);

		/* sleep & usleep only return EINTR & EINVAL, neither which we'd
	 	 * like to restart */
		if (nap.tv_sec)	 
			(void)sleep(nap.tv_sec);
		if (nap.tv_usec)	 
			(void)usleep(nap.tv_usec);
	}
}

void
catcher(int signo)
{
	/* stdio in signal handlers cause a race, instead we set a flag */
	if (signo == SIGINT)
		didsig = 1;
}

void
packet_stats()
{
	float bytes_sec = 0.0, mb_sec = 0.0;
	int pkts_sec = 0;

	if (gettimeofday(&end, NULL) < 0)
		err(1, "gettimeofday");

	timersub(&end, &begin, &begin);
	if (timerisset(&begin)) {
		if (bytes_sent) {
		bytes_sec = bytes_sent / (begin.tv_sec + (float)begin.tv_usec / 100000);
		mb_sec = (bytes_sec * 8) / (1024 * 1024);
		}
		if (pkts_sent)
		pkts_sec = pkts_sent / (begin.tv_sec + (float)begin.tv_usec / 100000);
	}

	fprintf(stderr, " %ld packets (%ld bytes) sent in %ld seconds\n",
		pkts_sent, bytes_sent, begin.tv_sec);
	fprintf(stderr, " %.1f bytes/sec %.2f megabits/sec %d packets/sec\n", 
		bytes_sec, mb_sec, pkts_sec);

	if (failed) {
		fprintf(stderr, 
			" %ld write attempts failed from full buffers and were repeated\n",
			failed);
	}
}

/*
 * converts a string representation of a MAC address, based on 
 * non-portable ether_aton() 
 */
void
mac2hex(const char *mac, char *dst, int len) 
{
	int i;
	long l;
	char *pp;

	if (len < 6)
		return;

	while (isspace(*mac))
		mac++;

	/* expect 6 hex octets separated by ':' or space/NUL if last octet */ 
	for (i = 0; i < 6; i++) { 
		l = strtol(mac, &pp, 16); 
		if (pp == mac || l > 0xFF || l < 0) 
			return; 
		if (!(*pp == ':' || (i == 5 && (isspace(*pp) || *pp == '\0')))) 
			return; 
		dst[i] = (u_char)l;
		mac = pp + 1; 
	}
}

void
version()
{
	fprintf(stderr, "Tcpreplay version: %s\n", VERSION);
	fprintf(stderr, "Compiled against Libnet %s\n", LIBNET_VERSION);
	exit(0);
}

void
usage()
{
	fprintf(stderr, "Usage: tcpreplay "
          "[-h|V] [-i pri int] [-j sec int] [-l loops] [-m multiplier] [-v]");
#ifdef DEBUG
	fprintf(stderr, " [-d]");
#endif
  	fprintf(stderr,"\n[-r rate] [-c cache|-C CIDR,...] [-u pad|trunc] [-I pri mac] [-J sec mac] <file>\n");
	exit(1);
}
