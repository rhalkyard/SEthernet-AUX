#include <time.h>
#include <netinet/if_ether.h>

#include "enc624j600_registers.h"

/* Calculate ENC624J600 base address from a slot number */
#define SE_BASE(slot) ((unsigned)0xf0000000 + (slot << 24))

/* Start of receive ring buffer, relative to base address */
#define SE_RXSTART 0x600

/* End of receive ring buffer, relative to base address */
#define SE_RXEND 0x6000

struct se_context {
	struct arpcom ac;
	unsigned char *base_address;		/* base address of chip */
	unsigned short rxptr;			/* read pointer for rx ring */
	volatile unsigned int reset_counter;
	volatile struct timeval last_reset;
	unsigned char mcast_refcount[64];	/* multicast reference counts */
};

/* Ring buffer header at the start of each packet */
struct se_rxheader {
	unsigned short next; 		/* offset of next packet */
	struct enc624j600_rsv rsv;	/* receive status vector */
};
