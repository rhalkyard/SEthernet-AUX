/* Driver for SEthernet/30 ethernet cards under A/UX
 *
 * Copyright 2024, Richard Halkyard
 */

#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/mbuf.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/reg.h>
#include <sys/slotmgr.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/user.h>
#include <vaxuba/ubavar.h>

#include <net/if.h>
#include <net/netisr.h>
#include <net/raw_cb.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>

#include <stddef.h>

#include "enc624j600_registers.h"
#include "if_se.h"

#ifndef VERSION
#define VERSION "<unknown>"
#endif

/* Maximum number of supported cards. 030 PDS only has interrupt lines for three
 * slots (9, A and B), so no point in supporting more than 3 */
#define N_SE 3

/* CRC32 polynomial for multicast hash calculation */
#define CRCPOLY 0x04c11db7

/* Size threshold for allocating a cluster mbuf vs. multiple regular mbufs */
#define MCLTHRESHOLD (MCLBYTES / 2)

/* Maximum number of receive-buffer error recoveries to take within a given time
 * period before giving up and disabling the interface */
#define MAX_RESETS (5)
#define RESET_COUNT_TIME (HZ * 30)

#ifdef DEBUG
/* If we declare our functions as static, they don't show up in the debugger.
 * Using a macro for static means that we can turn static-ness on and off with a
 * debug flag */
#define INTERNAL
#define DBGP(args) printf args
#else
#define INTERNAL static
#define DBGP(args)
#endif

/* external symbols that come from the kernel */
extern struct ifnet loif;
extern struct protosw ensw[];

/* external data set up for us by the kernel */
extern int secnt; /* Number of devices */
extern int seaddr[]; /* Slot numbers of devices indexed by unit number */

/* functions exported thru uba_driver struct */
INTERNAL int se_probe(/* struct uba_device *ui */);
INTERNAL int se_attach(/* struct uba_device *ui */);

/* functions exported thru ifnet struct */
INTERNAL int se_init(/* int unit */);
INTERNAL int se_ioctl(/* struct ifnet *ifp, int cmd, unsigned char * data */);
INTERNAL int se_output(/* struct ifnet *ifp, struct mbuf *m0,
			  struct sockaddr *dst */);

/* raw ethernet output */
INTERNAL int ren_output(/* struct mbuf *m0, struct socket *so */);

/* driver symbols exported to the kernel */
void seint(/* struct args *args */);
struct uba_device *seinfo[N_SE];
struct uba_driver sedriver = { se_probe, se_attach, (unsigned short *)0,
			       seinfo };

/* Internal functions */
INTERNAL void se_start(/* int unit */);
INTERNAL void se_update_linkstate(/* struct se_context *ctx */);
INTERNAL void se_reset_counter_clear( /* void * p */);
INTERNAL void se_rxbuf_reset(/* struct se_context *ctx */);
INTERNAL void se_rpkt(/* struct se_context *ctx */);
INTERNAL void se_update_multicast(/* struct se_context *ctx */);
INTERNAL int se_multicast_hash(/* unsigned char data[6] */);
INTERNAL int se_put(/* struct se_context * ctx, struct mbuf *m */);
INTERNAL void se_getbytes(/* struct se_context * ctx, unsigned char * dest, 
			     unsigned short len */);
INTERNAL struct mbuf *se_get(/* struct se_context * ctx */);

INTERNAL int se_units[16]; /* unit numbers of devices, indexed by slot number */
INTERNAL struct se_context se[N_SE];

#ifdef DEBUG
INTERNAL void se_hexdump(d, len)
unsigned char * d;
int len;
{
	int i;
	for (i = 0; i < len; i++) {
		if (d[i] < 0x10) {
			printf("0%x ", d[i]);
		} else {
			printf("%x ", d[i]);
		}
	}
}

INTERNAL void se_mdump(m) struct mbuf * m;
{
	int i;
	unsigned char b;
	printf("%d@%x ", m->m_len, m->m_off);
	while (m) {
		se_hexdump(mtod(m, unsigned char *), m->m_len);
		m = m->m_next;
		printf("-- ");
	}
	printf("\n");
}
#endif

/* Probe for presence of device, and reset it. Return 1 and populate device
 * address if present, 0 if not present */
INTERNAL int se_probe(ui)
struct uba_device *ui;
{
	struct se_context *ctx = &se[ui->ui_unit];
	unsigned char *addr = (unsigned char *)SE_BASE(seaddr[ui->ui_unit]);
	unsigned short magic = 0x1234;
	int i;

	if (ui->ui_unit < secnt && ui->ui_unit < N_SE && iocheck(&addr) != 0) {
		ctx->base_address = addr;

		/* Write and read-back a 'magic' value to the user data start
		 * pointer to verify that chip is present and functioning */
		ENC624J600_WRITE_REG(ctx->base_address, EUDAST,
					SWAPBYTES(magic));
		if (ENC624J600_READ_REG(ctx->base_address, EUDAST) !=
		    SWAPBYTES(magic)) {
			/* Couldn't write to chip */
			ctx->base_address = NULL;
			return 0;
		}
		/* Chip is present, wait for clock to be ready, then reset */
		while (!(ENC624J600_READ_REG(ctx->base_address, ESTAT) &
				ESTAT_CLKRDY)) {
		}
		ENC624J600_SET_BITS(ctx->base_address, ECON2, ECON2_ETHRST);

		/* Dummy delay while we wait for chip to come out of reset */
		for (i = 0; i < 1000; i++) { (void) i; }

		if (ENC624J600_READ_REG(ctx->base_address, EUDAST) ==
		    SWAPBYTES(magic)) {
			/* register was not cleared on reset! bail out! */
			ctx->base_address = NULL;
			return 0;
		}
	
		printf("se%d in slot %x (base addr %x)\n", ui->ui_unit,
		       seaddr[ui->ui_unit], addr);
		se_units[seaddr[ui->ui_unit]] = ui->ui_unit;

		return 1;

	} else {
		return 0;
	}
}

INTERNAL int se_attach(ui)
struct uba_device *ui;
{
	struct se_context *ctx = &se[ui->ui_unit];
	struct ifnet *ifp = &ctx->ac.ac_if;

	ifp->if_unit = ui->ui_unit;
	ifp->if_name = "se";
	ifp->if_mtu = ETHERMTU;
	ifp->if_init = se_init;
	ifp->if_ioctl = se_ioctl;
	ifp->if_output = se_output;
	ifp->if_flags = IFF_BROADCAST | IFF_NOTRAILERS;
	if_attach(ifp);
	ensw[0].pr_output = ren_output;
	bzero(ctx->mcast_refcount, 64);
	return 0;
}

/* Initialise chip and bring interface up; we can assume it has already been
 * reset by se_probe() */
INTERNAL int se_init(unit)
int unit;
{
	struct se_context *ctx = &se[unit];
	struct ifnet *ifp = &ctx->ac.ac_if;
	int i, s;
	unsigned short *words;
	unsigned short tmp, flow_hwm, flow_lwm, rxbuf_size;

	/* can't init yet, address not known */
	if (ifp->if_addrlist == (struct ifaddr *)0) {
		return -1;
	}

	/* Set up receive buffer from end of transmit buffer to end of RAM */
	ENC624J600_WRITE_REG(ctx->base_address, ERXST, SWAPBYTES(SE_RXSTART));
	ENC624J600_WRITE_REG(ctx->base_address, ERXTAIL,
			     SWAPBYTES(SE_RXEND - 2));

	/* Start receive FIFO read pointer at beginning of buffer */
	ctx->rxptr = SE_RXSTART;

	/* Set up flow control parameters. We only enable flow control for
	 * full-duplex links, since half-duplex flow control operates by jamming
	 * the medium, which is an extremely antisocial thing to do on
	 * shared-media links (such as if connected to a hub rather than a
	 * switch).
	 *
	 * The high- and low-water-mark parameters (assert flow control at 3/4
	 * full, deassert at 1/2 full) are completely made up based on gut
	 * instinct. Should probably tune them at some point. */
	rxbuf_size = SE_RXEND - SE_RXSTART;
	flow_hwm = (rxbuf_size - (rxbuf_size / 4)) / 96;
	flow_lwm = (rxbuf_size / 2) / 96;
	tmp = (flow_hwm << ERXWM_RXFWM_SHIFT) | (flow_lwm << ERXWM_RXEWM_SHIFT);
	ENC624J600_WRITE_REG(ctx->base_address, ERXWM, tmp);

	/* Set up 25MHz clock output (used by glue logic for timing control). */
	tmp = ENC624J600_READ_REG(ctx->base_address, ECON2);
	tmp &= ~ECON2_COCON_MASK;
	tmp |= 0x2 << ECON2_COCON_SHIFT;
	ENC624J600_WRITE_REG(ctx->base_address, ECON2, tmp);

	/* Set up Link/Activity LEDs */
	tmp = ENC624J600_READ_REG(ctx->base_address, EIDLED);
	tmp &= ~(EIDLED_LACFG_MASK | EIDLED_LBCFG_MASK);
	tmp |= (0x2 << EIDLED_LACFG_SHIFT) | /* LED A indicates link state */
	       (0x6 << EIDLED_LBCFG_SHIFT); /* LED B indicates activity */
	ENC624J600_WRITE_REG(ctx->base_address, EIDLED, tmp);

	/* Set local ethernet address */
	words = (unsigned short *)ctx->ac.ac_enaddr;
	words[0] = ENC624J600_READ_REG(ctx->base_address, MAADR1);
	words[1] = ENC624J600_READ_REG(ctx->base_address, MAADR2);
	words[2] = ENC624J600_READ_REG(ctx->base_address, MAADR3);
	localetheraddr(ctx->ac.ac_enaddr, NULL);

	/* copy multicast hash table to chip */
	se_update_multicast(ctx);

	/* Sync MAC duplex configuration with autonegotiated values from PHY */
	se_update_linkstate(ctx);

	/* Set receive configuration (reject bad-CRC and runt frames, accept
	 * unicast-to-us, broadcast, and multicast hash matches) */
	ENC624J600_WRITE_REG(ctx->base_address, ERXFCON,
			     ERXFCON_CRCEN | ERXFCON_RUNTEN | ERXFCON_UCEN |
				     ERXFCON_BCEN | ERXFCON_HTEN);

	/* Enable packet reception */
	s = splimp();
	ENC624J600_SET_BITS(ctx->base_address, ECON1, ECON1_RXEN);

	/* Mark interface as running */
	ifp->if_flags |= IFF_RUNNING;

	/* start transmission if we have packets waiting */
	if (ctx->ac.ac_if.if_snd.ifq_head) {
		se_start(unit);
	}

	/* enable interrupts */
	ENC624J600_SET_BITS(ctx->base_address, EIE,
			    EIE_INTIE | EIE_LINKIE | EIE_PKTIE | EIE_RXABTIE |
				    EIE_PCFULIE | EIE_TXIE | EIE_TXABTIE);
	
	splx(s);
	printf("se%d: init complete. Driver version %s", unit, VERSION);
	DBGP((" DEBUG BUILD ifp=%x", unit, ifp));
	printf("\n");
	return 0;
}

/* Start a transmit operation */
INTERNAL void se_start(unit)
int unit;
{
	int len;
	struct se_context *ctx = &se[unit];
	struct mbuf *m;

	/* Bail out if a transmit is already in progress; the queue will be
	serviced by the ISR instead */
	if ((ENC624J600_READ_REG(ctx->base_address, ECON1) & ECON1_TXRTS)) {
		return;
	}

	/* Take a packet off the send queue */
	IF_DEQUEUE(&ctx->ac.ac_if.if_snd, m);
	if (m == 0) {
		return;
	}

	/* Write packet to transmit buffer */
	len = se_put(ctx, m);

	/* Ready, set, go! */
	ENC624J600_WRITE_REG(ctx->base_address, ETXST, 0);
	ENC624J600_WRITE_REG(ctx->base_address, ETXLEN, SWAPBYTES(len));
	ENC624J600_SET_BITS(ctx->base_address, ECON1, ECON1_TXRTS);
}

/* Prepare an mbuf chain for transmission and queue it on the interface */
INTERNAL int se_output(ifp, m0, dst)
struct ifnet *ifp;
struct mbuf *m0;
struct sockaddr *dst;
{
	int type, s, error;
	unsigned char edst[6];
	struct in_addr idst;
	register struct se_context *ctx = &se[ifp->if_unit];
	register struct mbuf *m = m0;
	struct mbuf *mcopy = (struct mbuf *)0;
	register struct ether_header *header;
	int usetrailers;

	if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) !=
	    (IFF_UP | IFF_RUNNING)) {
		/* Don't transmit on a down interface */
		error = ENETDOWN;
		goto bad;
	}

	switch (dst->sa_family) {
	case AF_INET:
		idst = ((struct sockaddr_in *)dst)->sin_addr;
		if (!arpresolve(&ctx->ac, m, &idst, edst, &usetrailers)) {
			/* arp takes ownership of mbuf */
			return 0;
		}

		type = ETHERTYPE_IP;
		if ((in_lnaof(idst) == INADDR_ANY) || in_broadcast(idst) ||
		    (in_lnaof(idst) == INADDR_BROADCAST)) {
		    /* copy broadcasts to loopback interface */
			mcopy = m_copy(m, 0, (int)M_COPYALL);
		}
		break;
	case AF_UNSPEC:
		header = (struct ether_header *)dst->sa_data;
		bcopy((unsigned char *)header->ether_dhost,
		      (unsigned char *)edst,
		      sizeof(edst));
		type = header->ether_type;
		break;
#ifdef APPLETALK
	case AF_APPLETALK:
		header = mtod(m, struct ether_header *);
		type = header->ether_type;
		if (type >= 0 && type <= ETHERMTU) {
			goto gotheader;
		} else {
			error = EINVAL;
			goto bad;
		}
#endif
	case AF_ETHERLINK:
		header = mtod(m, struct ether_header *);
		goto gotheader;

	default:
		printf("se%d: can't handle af%d\n", ifp->if_unit,
		       dst->sa_family);
		error = EAFNOSUPPORT;
		goto bad;
	}

	/* Add local net header.  If no space in first mbuf (or first mbuf is
	 * cluster), allocate another */
	if (m->m_off > MMAXOFF ||
	    MMINOFF + sizeof(struct ether_header) > m->m_off) {
		m = m_get(M_DONTWAIT, MT_HEADER);
		if (m == 0) {
			error = ENOBUFS;
			goto bad;
		}
		m->m_next = m0;
		m->m_off = MMINOFF;
		m->m_len = sizeof(struct ether_header);
	} else {
		m->m_off -= sizeof(struct ether_header);
		m->m_len += sizeof(struct ether_header);
	}

	header = mtod(m, struct ether_header *);
	header->ether_type = type;
	bcopy((unsigned char *)edst, (unsigned char *)header->ether_dhost,
	      sizeof(edst));

gotheader:
	/* Fill in the source address. */
	bcopy((unsigned char *)ctx->ac.ac_enaddr,
	      (unsigned char *)header->ether_shost,
	      sizeof(header->ether_shost));

	/* Queue message on interface, and start output if interface not yet
	* active. */
	s = splimp();
	if (IF_QFULL(&ifp->if_snd)) {
		IF_DROP(&ifp->if_snd);
		splx(s);
		goto bad;
	}
	IF_ENQUEUE(&ifp->if_snd, m);
	se_start(ifp->if_unit);
	splx(s);
	return (mcopy ? looutput(&loif, mcopy, dst) : 0);

bad:
	m_freem(m0);
	if (mcopy) {
		m_freem(mcopy);
	}
	return (error);
}

INTERNAL int ren_output(m, so)
struct mbuf *m;
struct socket *so;
{
	struct rawcb *rp = sotorawcb(so);
	struct ifaddr *ifa;
	struct ifnet *ifp;
	int error;

	if (rp == (struct rawcb *)0) {
		error = ENOPROTOOPT;
		goto bad;
	}
	switch (rp->rcb_proto.sp_family) {
#ifdef ETHERLINK
	case AF_ETHERLINK: {
		struct sockaddr_in inether, *sin;
		if ((rp->rcb_flags & RAW_LADDR) == 0) {
			error = EADDRNOTAVAIL;
			goto bad;
		}
		bzero((unsigned char *)&inether, (unsigned)sizeof(inether));
		inether.sin_family = AF_INET;
		sin = (struct sockaddr_in *)&rp->rcb_laddr;
		inether.sin_addr = sin->sin_addr;
		if (inether.sin_addr.s_addr &&
		    (ifa = ifa_ifwithaddr((struct sockaddr *)&inether)) == 0) {
			error = EADDRNOTAVAIL;
			goto bad;
		}
		break;
	}
#endif
	default:
		error = EPROTOTYPE;
		goto bad;
		break;
	}
	ifp = ifa->ifa_ifp;
	if (ifp) {
		return ((*ifp->if_output)(ifp, m, &rp->rcb_laddr));
	}
	error = EADDRNOTAVAIL;
bad:
	if (m != 0) {
		m_freem(m);
	}
	return error;
}

/* Interrupt service routine */
void seint(args)
struct args *args;
{
	int unit = se_units[args->a_dev];
	int s;
	register struct se_context *ctx = &se[unit];
	register unsigned short *ir;
	unsigned short tail;

	if (unit < 0 || unit > N_SE) {
		printf("se: interrupt from mystery unit #%d\n", unit);
		panic("se");
	}

	if (ctx->base_address == NULL) {
		printf("se: interrupt from uninitialised unit #%d\n", unit);
		panic("se");
	}

	ir = ENC624J600_REG(ctx->base_address, EIR);

	/* Link state has changed; update flow control and duplex parameters */
	if (*ir & EIR_LINKIF) {
		printf("se%d: link %s\n", unit,
		       (ENC624J600_READ_REG(ctx->base_address, ESTAT) &
			ESTAT_PHYLNK) ? "up" : "down");
		se_update_linkstate(ctx);
		ENC624J600_CLEAR_BITS(ctx->base_address, EIR, EIR_LINKIF);
	}

	/* Transmit complete or abort */
	if (*ir & (EIR_TXIF | EIR_TXABTIF)) {
		if (*ir & EIR_TXABTIF) {
			printf("se%d: transmit abort\n");
			ctx->ac.ac_if.if_oerrors++;
		} else {
			ctx->ac.ac_if.if_opackets++;
		}
		ENC624J600_CLEAR_BITS(ctx->base_address, EIR,
				      EIR_TXIF | EIR_TXABTIF);
		/* Start transmitting the next queued packet */
		s = splimp();
		if (ctx->ac.ac_if.if_snd.ifq_head) {
			se_start(unit);
		}
		splx(s);
	}

	/* Recieve abort interrupt. Not much we can do here except note it */
	if (*ir & EIR_RXABTIF) {
		ENC624J600_CLEAR_BITS(ctx->base_address, EIR, EIR_RXABTIF);
		printf("se%d: receive overflow, packet(s) dropped\n", unit);
		ctx->ac.ac_if.if_ierrors++;
	}

	/* Handle any received packets */
	while (*ir & EIR_PKTIF) {
		ENC624J600_SET_BITS(ctx->base_address, ECON1, ECON1_PKTDEC);
		se_rpkt(ctx);
	}

	return;
}

struct sockaddr redst = { AF_ETHERLINK };
struct sockaddr resrc = { AF_ETHERLINK };
struct sockproto reproto = { PF_ETHERLINK };

/* Packet-reception handler */
INTERNAL void se_rpkt(ctx)
struct se_context *ctx;
{
	struct ifnet * ifp = &ctx->ac.ac_if;
	struct ether_header * eh;
	register unsigned short len;
	register struct mbuf *m;
	struct ifqueue *inq;
	register unsigned short type;
	unsigned short next, tail;
	int s;

	ifp->if_ipackets++;

	/* se_get returns an mbuf chain with the ethernet header in an mbuf on
	 * its own, followed by the ethernet payload in subsequent mbufs. The
	 * header mbuf is pre-offset so that the interface pointer may be
	 * prepended to it. This structure facilitates the inclusion (for
	 * AppleTalk) or stripping (for TCP/IP) of the ethernet header while
	 * keeping the payload aligned to the start of an mbuf (which NFS seems
	 * to expect). */
	m = se_get(ctx);
	if (m == 0) {
		printf("se%d: Packet read failed.\n", ifp->if_unit);
		goto done;
	}

	eh = mtod(m, struct ether_header *);
	type = eh->ether_type;

	/* Discard ethernet header for non-802.3 packets */
	if (type > ETHERMTU) {
		m->m_off += sizeof(struct ether_header);
		m->m_len -= sizeof(struct ether_header);
	}

	/* Prepend interface pointer */
	m->m_off -= sizeof(struct ifnet *);
	m->m_len += sizeof(struct ifnet *);
	*mtod(m, struct ifnet **) = ifp;

	switch (type) {
	case ETHERTYPE_IP:
		schednetisr(NETISR_IP);
		inq = &ipintrq;
		goto enqueue;

	case ETHERTYPE_ARP:
		arpinput(&ctx->ac, m);
		goto done;

	case ETHERTYPE_REVARP:
		revarpinput(&ctx->ac, m);
		goto done;
	default:
#ifdef APPLETALK
		if (type >= 0 && type <= ETHERMTU && NETISR_ET != NULL) {
			if (type < 60) {
				/* Discard padding for short packets */
				m->m_next->m_len = type;
			}

			/* ugh. Appletalk expects the 8-byte LLC header to be
			 * contiguous with the ethernet header. */
			m = m_pullup(m, sizeof(struct ifnet *)
				     + sizeof(struct ether_header) + 8);

			schednetisr(*NETISR_ET);
			inq = &etintrq;
			goto enqueue;
		} else if (NETISR_ET = NULL) {
			m_freem(m);
			goto done;
		}
#endif
#ifdef ETHERLINK
		reproto.sp_protocol = type;
		/* source address = WHOLE ethernet header */
		bcopy((unsigned char *)eh->ether_dhost,
		      (unsigned char *)resrc.sa_data,
		      sizeof(struct ether_header));
		/* destination address = actual destination */
		bcopy((unsigned char *)eh->ether_dhost,
		      (unsigned char *)redst.sa_data,
		      sizeof(eh->ether_dhost));

		/* raw_input takes the payload only, no interface ptr */
		raw_input(m->m_next, &reproto, &resrc, &redst);
		m_free(m);
		goto done;
#else
		m_freem(m);
		goto done;
#endif
	}
enqueue:
	s = splimp();
	if (IF_QFULL(inq)) {
		IF_DROP(inq);
		splx(s);
		m_freem(m);
		return;
	}
	IF_ENQUEUE(inq, m);
	splx(s);
done:
	return;
}

/* ioctl handler */
INTERNAL int se_ioctl(ifp, cmd, data)
struct ifnet *ifp;
int cmd;
unsigned char * data;
{
	struct ifaddr *ifa = (struct ifaddr *)data;
	struct sockaddr *sa = (struct sockaddr *)data;
	struct se_context *ctx = &se[ifp->if_unit];
	int s = splimp();
	int error = 0;
	int bit;

	DBGP(("se%d: ioctl %x from pid %d\n",
		ifp->if_unit, cmd, u.u_procp->p_pid));
	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		switch (ifa->ifa_addr.sa_family) {
		case AF_INET:
			/* Initalize the interface. This includes setting the
			 * ethernet address for the interface. */
			se_init(ifp->if_unit);
			((struct arpcom *)ifp)->ac_ipaddr =
				IA_SIN(ifa)->sin_addr;
			arpwhohas((struct arpcom *)ifp, &IA_SIN(ifa)->sin_addr);
			break;

		default:
			error = EINVAL;
			break;
		}
		break;
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if (!(ifp->if_flags & IFF_RUNNING)) {
				se_init(ifp->if_unit);
			} else {
				ENC624J600_SET_BITS(ctx->base_address, ECON1,
						    ECON1_RXEN);
			}
		} else {
			ifp->if_flags &= ~IFF_RUNNING;
			ENC624J600_CLEAR_BITS(ctx->base_address, ECON1,
					      ECON1_RXEN);
		}

		if (!(ifp->if_flags & IFF_NOTRAILERS)) {
			/* We don't support trailers, don't allow the
			 * no-trailers bit to be cleared */
			DBGP(("se%d: pid %d tried to enable trailers\n",
			      ifp->if_unit, u.u_procp->p_pid));
			ifp->if_flags |= IFF_NOTRAILERS;
		}
		break;
#ifdef APPLETALK
	case SIOCSMAR:
		/* Subscribe to a multicast address */
		bit = se_multicast_hash(sa->sa_data);
		ctx->mcast_refcount[bit]++;
		se_update_multicast(ctx);
		break;
	case SIOCUMAR:
		/* Unsubscribe from a multicast address */
		bit = se_multicast_hash(sa->sa_data);
		if (ctx->mcast_refcount[bit] > 0) {
			ctx->mcast_refcount[bit]--;
			se_update_multicast(ctx);
		}
		break;
	case SIOCGMAR:
		/* This ioctl appears to just dump the multicast hash table as
		 * stored in the ethernet controller's registers. The ENC624J600
		 * hashes addresses differently to the DP8390 so this probably
		 * won't work as it's supposed to :/ */
		DBGP(("se%d: pid %d dumped the multicast table.\n",
		      ifp->if_unit, u.u_procp->p_pid));
		{
			int i;
			unsigned short *words;
			words = (unsigned short *)sa->sa_data;
			words[0] = ENC624J600_READ_REG(ctx->base_address, EHT1);
			words[1] = ENC624J600_READ_REG(ctx->base_address, EHT2);
			words[2] = ENC624J600_READ_REG(ctx->base_address, EHT3);
			words[3] = ENC624J600_READ_REG(ctx->base_address, EHT4);

			for (i = 0; i < 4; i++) {
				words[i] = SWAPBYTES(words[i]);
			}
		}
		break;
#endif
	default:
		DBGP(("se%d: pid %d issued unknown ioctl 0x%x\n",
		      ifp->if_unit, u.u_procp->p_pid, cmd));
		error = EINVAL;
		break;
	}
	splx(s);
	return error;
}

/* Read autonegotiated full/half-duplex status from PHY, set MAC duplex and
 * back-to-back interpacket gap as appropriate. Call on initial startup and
 * whenever link stage changes. */
INTERNAL void se_update_linkstate(ctx)
struct se_context *ctx;
{
	/* Wait for flow control state machine to be idle before changing duplex
	 * mode or flow control settings */
	while (!(ENC624J600_READ_REG(ctx->base_address, ESTAT) &
		 ESTAT_FCIDLE)) {
	};

	if (ENC624J600_READ_REG(ctx->base_address, ESTAT) & ESTAT_PHYDPX) {
		/* Full duplex */
		ENC624J600_SET_BITS(ctx->base_address, MACON2, MACON2_FULDPX);
		ENC624J600_WRITE_REG(ctx->base_address, MABBIPG,
				     0x15 << MABBIPG_BBIPG_SHIFT);
		/* Enable automtaic flow control */
		ENC624J600_SET_BITS(ctx->base_address, ECON2, ECON2_AUTOFC);
	} else {
		/* Half duplex */
		ENC624J600_CLEAR_BITS(ctx->base_address, MACON2, MACON2_FULDPX);
		ENC624J600_WRITE_REG(ctx->base_address, MABBIPG,
				     0x12 << MABBIPG_BBIPG_SHIFT);
		/* Disable automatic flow control */
		ENC624J600_CLEAR_BITS(ctx->base_address, ECON2, ECON2_AUTOFC);
		/* Ensure flow control is deasserted */
		ENC624J600_CLEAR_BITS(ctx->base_address, ECON1,
				      ECON1_FCOP1 | ECON1_FCOP1);
	}
}

/* Update the ENC624J600 multicast hash table from our reference-count array */
INTERNAL void se_update_multicast(ctx)
struct se_context *ctx;
{
	unsigned short reg;
	unsigned short bit;
	unsigned short table[4];

	/* Build multicast hash table bits */
	for (reg = 0; reg < 4; reg++) {
		table[reg] = 0;
		for (bit = 0; bit < 16; bit++) {
			if (ctx->mcast_refcount[(reg * 16) + bit]) {
				table[reg] |= BIT(bit);
			}
		}
	}

	ENC624J600_WRITE_REG(ctx->base_address, EHT1, SWAPBYTES(table[0]));
	ENC624J600_WRITE_REG(ctx->base_address, EHT2, SWAPBYTES(table[1]));
	ENC624J600_WRITE_REG(ctx->base_address, EHT3, SWAPBYTES(table[2]));
	ENC624J600_WRITE_REG(ctx->base_address, EHT4, SWAPBYTES(table[3]));
}

/* Calculate a bit position in the multicast hash table. Bits 28:23 of the CRC32
 * of the destination address. */
INTERNAL int se_multicast_hash(addr)
unsigned char *addr;
{
	int i, j;
	unsigned int crc;
	unsigned char byte;

	crc = 0xffffffff;
	for (i = 0; i < 6; i++) {
		byte = addr[i];
		for (j = 0; j < 8; j++) {
			if ((byte & 1) ^ (crc >> 31)) {
				crc <<= 1;
				crc ^= CRCPOLY;
			} else {
				crc <<= 1;
			}
			byte >>= 1;
		}
	}

	return (crc >> 23) & 0x3f;
}

/* Write an mbuf chain to the transmit buffer */
INTERNAL int se_put(ctx, m)
struct se_context * ctx;
struct mbuf *m;
{
	register struct mbuf *mp;
	register int totlen;
	register unsigned char *bp;

	for (bp = ctx->base_address, mp = m, totlen = 0; mp; mp = mp->m_next) {
		register unsigned mlen = mp->m_len;

		totlen += mlen;
		if (mlen == 0) {
			continue;
		}
		bcopy(mtod(mp, unsigned char *), bp, mlen);
		bp += mlen;
	}
	m_freem(m);
	return totlen;
}

/* Called by timeout() at end of se_rxbuf_reset(). Clears reset counter after a
 * a while so we can differentiate between a one-off error or a card/driver
 * that's gone haywire */
INTERNAL void se_reset_counter_clear(p)
void *p;
{
	struct se_context * ctx = (struct se_context *) p;
	ctx->reset_counter = 0;

	printf("se%d: reset counter cleared\n", ctx->ac.ac_if.if_unit);
}

/* Attempt to recover from loss-of-state errors by re-initialising the receive
 * buffer pointers. Any pending packets will get dropped in the process, but I
 * guess it beats either panic-ing or blindly continuing. If called more than
 * MAX_RESETS times within RESET_COUNT_TIME ticks, then something has probably
 * gone very wrong with either the driver or the hardware, so we leave the
 * interface disabled. */
INTERNAL void se_rxbuf_reset(ctx)
struct se_context *ctx;
{
	int dropcnt = 0;
	/* Disable packet reception while we fiddle with the buffer */
	ENC624J600_CLEAR_BITS(ctx->base_address, ECON1, ECON1_RXEN);

	untimeout(se_reset_counter_clear, ctx);
	if (ctx->reset_counter++ > MAX_RESETS) {
		/* give up and leave interface disabled */
		printf("se%d: in jail for buffer crimes\n",
		       ctx->ac.ac_if.if_unit);
		return;
	}

	printf("se%d: dazed and confused, but trying to continue. rxptr=%x\n",
	       ctx->ac.ac_if.if_unit, ctx->rxptr);

	/* Wait for any in-progress receive to finish */
	while (ENC624J600_READ_REG(ctx->base_address, ESTAT) & ESTAT_RXBUSY) {};

	/* Clear all pending packets */
	while (ENC624J600_READ_REG(ctx->base_address, EIR) & EIR_PKTIF) {
		ENC624J600_SET_BITS(ctx->base_address, ECON1, ECON1_PKTDEC);
		dropcnt++;
	}
	if (dropcnt) {
		printf("se%d: dropped %d packets during rx buffer recovery\n",
		ctx->ac.ac_if.if_unit, dropcnt);
	}

	/* Restore buffer pointers to their initial conditions. */
	ENC624J600_WRITE_REG(ctx->base_address, ERXST, SWAPBYTES(SE_RXSTART));
	ENC624J600_WRITE_REG(ctx->base_address, ERXTAIL,
				SWAPBYTES(SE_RXEND - 2));
	ctx->rxptr = SE_RXSTART;
	
	/* Good to go, post a callback to clear reset counter if no more resets
	 * happen for a while */
	timeout(se_reset_counter_clear, ctx, RESET_COUNT_TIME);
	ENC624J600_SET_BITS(ctx->base_address, ECON1, ECON1_RXEN);
}

/* Read len bytes from the receive ring buffer, wrapping around if necessary. */
INTERNAL void se_getbytes(ctx, dest, len)
struct se_context * ctx;
register unsigned char * dest;
unsigned short len;
{
	register unsigned char * base = ctx->base_address;
	register unsigned short rxptr = ctx->rxptr;
	unsigned short remainder;

	if (rxptr + len < SE_RXEND) {
		bcopy(base + rxptr, dest, len);
		rxptr += len;
	} else {
		bcopy(base + rxptr, dest, SE_RXEND - rxptr);
		dest += SE_RXEND - rxptr;
		remainder = rxptr + len - SE_RXEND;
		bcopy(base + SE_RXSTART, dest, remainder);
		rxptr = SE_RXSTART + remainder;
	}
	ctx->rxptr = rxptr;
}

/* Pull a packet off the receive ring, updating ring-buffer pointers accordingly */
INTERNAL struct mbuf * se_get(ctx)
struct se_context *ctx;
{
	struct mbuf *top;
	register struct mbuf *m, *mp;
	struct se_rxheader h;
	register unsigned short len;
	unsigned short next, tail;

	/* A packet will always start on a 16-bit boundary within the receive
	 * buffer area. If not, then something's wrong and nothing good will
	 * come of trying to go further. */
	if (ctx->rxptr % 2 || ctx->rxptr < SE_RXSTART || ctx->rxptr > SE_RXEND) {
		printf("se%d: bogus rxptr %x\n", ctx->ac.ac_if.if_unit,
		       ctx->rxptr);
		se_rxbuf_reset(ctx);
		return 0;
	}

	se_getbytes(ctx, (unsigned char *) &h, sizeof(struct se_rxheader));
	len = SWAPBYTES(h.rsv.pkt_len_le) - 4; /* discard checksum */
	next = SWAPBYTES(h.next);

	/* Apply same checks as above to the next-packet pointer. This is a
	 * "can't happen" situation if the driver and chip are functioning
	 * correctly, but check anyway just in case I've screwed something up */
	if (next % 2 || next < SE_RXSTART || next >= SE_RXEND) {
		printf("se%d: bogus next-packet pointer %x.\n",
		       ctx->ac.ac_if.if_unit, next);
		se_rxbuf_reset(ctx);
		return 0;
	}

	/* The ENC624J600 will drop runt and too-long frames. If we read a bad
	 * length, then either the chip or driver is misbehaving. */
	if (len + sizeof(struct ether_header) < ETHERMIN
	    || len + sizeof(struct ether_header) > ETHERMTU) {
		printf("se%d: bogus packet length %d\n", ctx->ac.ac_if.if_unit,
		       len);
		se_rxbuf_reset(ctx);
		return 0;
	}
 
	MGET(top, M_DONTWAIT, MT_DATA);
	if (top == 0) {
		DBGP(("se_get failed to get first mbuf\n"));
		goto done;
	}

	/* leave space for the interface pointer */
	top->m_off += sizeof(struct ifnet *);
	top->m_len = sizeof(struct ether_header);

	se_getbytes(ctx, mtod(top, unsigned char *),
		    sizeof(struct ether_header));
	len -= sizeof(struct ether_header);

	/* get any remaining data into additional mbufs */
	mp = top;
	while (len > 0) {
		MGET(m, M_DONTWAIT, MT_DATA);
		if (m == 0) {
			DBGP(("se_get: failed to chain mbuf\n"));
			m_freem(top);
			top = 0;
			goto done;
		}
		m->m_len = MLEN;
		mp->m_next = m;
		mp = m;

		if (len > MCLTHRESHOLD) {
			MCLGET(m);
		}
		/* If we got a cluster with MCLGET(m), then m_len will have
		 * been set to the cluster size */
		m->m_len = MIN(m->m_len, len);

		se_getbytes(ctx, mtod(m, unsigned char *), m->m_len);
		len -= m->m_len;
	}
done:
	/* tail of receive ring buffer must be at least 2 bytes behind our read
	 * pointer */
	tail = next - 2;
	if (tail < SE_RXSTART) {
		tail = SE_RXEND - 2;
	}
	ENC624J600_WRITE_REG(ctx->base_address, ERXTAIL, SWAPBYTES(tail));
	ctx->rxptr = next;
	return top;
}
