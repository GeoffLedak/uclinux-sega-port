/*
 *	AX.25 release 035
 *
 *	This code REQUIRES 1.2.1 or higher/ NET3.029
 *
 *	This module:
 *		This module is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	Most of this code is based on the SDL diagrams published in the 7th
 *	ARRL Computer Networking Conference papers. The diagrams have mistakes
 *	in them, but are mostly correct. Before you modify the code could you
 *	read the SDL diagrams as the code is not obvious and probably very
 *	easy to break;
 *
 *	History
 *	AX.25 028a	Jonathan(G4KLX)	New state machine based on SDL diagrams.
 *	AX.25 028b	Jonathan(G4KLX) Extracted AX25 control block from
 *					the sock structure.
 *	AX.25 029	Alan(GW4PTS)	Switched to KA9Q constant names.
 *			Jonathan(G4KLX)	Added IP mode registration.
 *	AX.25 030	Jonathan(G4KLX)	Added AX.25 fragment reception.
 *					Upgraded state machine for SABME.
 *					Added arbitrary protocol id support.
 *	AX.25 031	Joerg(DL1BKE)	Added DAMA support
 *			HaJo(DD8NE)	Added Idle Disc Timer T5
 *			Joerg(DL1BKE)   Renamed it to "IDLE" with a slightly
 *					different behaviour. Fixed defrag
 *					routine (I hope)
 *	AX.25 032	Darryl(G7LED)	AX.25 segmentation fixed.
 *	AX.25 033	Jonathan(G4KLX)	Remove auto-router.
 *					Modularisation changes.
 *			Joerg(DL1BKE)	Fixed 2.0.x specific IP over AX.25 problem.
 */

#include <linux/config.h>
#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <net/ax25.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/ip.h>			/* For ip_rcv */
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/interrupt.h>

static int ax25_rx_iframe(ax25_cb *, struct sk_buff *);

/*
 *	Given a fragment, queue it on the fragment queue and if the fragment
 *	is complete, send it back to ax25_rx_iframe.
 */
static int ax25_rx_fragment(ax25_cb *ax25, struct sk_buff *skb)
{
	struct sk_buff *skbn, *skbo;

	if (ax25->fragno != 0) {
		if (!(*skb->data & AX25_SEG_FIRST)) {
			if ((ax25->fragno - 1) == (*skb->data & AX25_SEG_REM)) {
				/* Enqueue fragment */
				ax25->fragno = *skb->data & AX25_SEG_REM;
				skb_pull(skb, 1);	/* skip fragno */
				ax25->fraglen += skb->len;
				skb_queue_tail(&ax25->frag_queue, skb);

				/* Last fragment received ? */
				if (ax25->fragno == 0) {
					if ((skbn = alloc_skb(AX25_MAX_HEADER_LEN + ax25->fraglen, GFP_ATOMIC)) == NULL) {
						while ((skbo = skb_dequeue(&ax25->frag_queue)) != NULL)
							kfree_skb(skbo, FREE_READ);
						return 1;
					}

					skb_reserve(skbn, AX25_MAX_HEADER_LEN);

					skbn->free  = 1;
					skbn->dev   = ax25->device;
					skbn->h.raw = skbn->data;

					/* Copy data from the fragments */
					while ((skbo = skb_dequeue(&ax25->frag_queue)) != NULL) {
						memcpy(skb_put(skbn, skbo->len), skbo->data, skbo->len);
						kfree_skb(skbo, FREE_READ);
					}

					ax25->fraglen = 0;

					if (ax25_rx_iframe(ax25, skbn) == 0)
						kfree_skb(skbn, FREE_READ);
				}

				return 1;
			}
		}
	} else {
		/* First fragment received */
		if (*skb->data & AX25_SEG_FIRST) {
			while ((skbo = skb_dequeue(&ax25->frag_queue)) != NULL)
				kfree_skb(skbo, FREE_READ);
			ax25->fragno = *skb->data & AX25_SEG_REM;
			skb_pull(skb, 1);		/* skip fragno */
			ax25->fraglen = skb->len;
			skb_queue_tail(&ax25->frag_queue, skb);
			return 1;
		}
	}

	return 0;
}

/*
 *	This is where all valid I frames are sent to, to be dispatched to
 *	whichever protocol requires them.
 */
static int ax25_rx_iframe(ax25_cb *ax25, struct sk_buff *skb)
{
	int (*func)(struct sk_buff *, ax25_cb *);
	struct sk_buff *skbn;
	volatile int queued = 0;
	unsigned char pid;

	if (skb == NULL) return 0;

	ax25->idletimer = ax25->idle;

	pid = *skb->data;

#ifdef CONFIG_INET
	if (pid == AX25_P_IP) {
		if ((skbn = skb_copy(skb, GFP_ATOMIC)) != NULL) {
			kfree_skb(skb, FREE_READ);
			skb = skbn;
		}
		skb_pull(skb, 1);	/* Remove PID */
		skb->h.raw = skb->data;
		ip_rcv(skb, ax25->device, NULL);	/* Wrong ptype */
		return 1;
	}
#endif
	if (pid == AX25_P_SEGMENT) {
		skb_pull(skb, 1);	/* Remove PID */
		return ax25_rx_fragment(ax25, skb);
	}

	if ((func = ax25_protocol_function(pid)) != NULL) {
		skb_pull(skb, 1);	/* Remove PID */
		return (*func)(skb, ax25);
	}

	if (ax25->sk != NULL && ax25_dev_get_value(ax25->device, AX25_VALUES_CONMODE) == 2) {
		if ((!ax25->pidincl && ax25->sk->protocol == pid) || ax25->pidincl) {
			if (sock_queue_rcv_skb(ax25->sk, skb) == 0)
				queued = 1;
			else
				ax25->condition |= AX25_COND_OWN_RX_BUSY;
		}
	}

	return queued;
}

/*
 *	State machine for state 1, Awaiting Connection State.
 *	The handling of the timer(s) is in file ax25_timer.c.
 *	Handling of state 0 and connection release is in ax25.c.
 */
static int ax25_state1_machine(ax25_cb *ax25, struct sk_buff *skb, int frametype, int pf, int type, int dama)
{
	switch (frametype) {
		case AX25_SABM:
			ax25->modulus = AX25_MODULUS;
			ax25->window  = ax25_dev_get_value(ax25->device, AX25_VALUES_WINDOW);
			ax25_send_control(ax25, AX25_UA, pf, AX25_RESPONSE);
			break;

		case AX25_SABME:
			ax25->modulus = AX25_EMODULUS;
			ax25->window  = ax25_dev_get_value(ax25->device, AX25_VALUES_EWINDOW);
			ax25_send_control(ax25, AX25_UA, pf, AX25_RESPONSE);
			break;

		case AX25_DISC:
			ax25_send_control(ax25, AX25_DM, pf, AX25_RESPONSE);
			break;

		case AX25_UA:
			if (pf || dama) {
				if (dama) ax25_dama_on(ax25); /* bke */
				ax25_calculate_rtt(ax25);
				ax25->t1timer = 0;
				ax25->t3timer = ax25->t3;
				ax25->idletimer = ax25->idle;
				ax25->vs      = 0;
				ax25->va      = 0;
				ax25->vr      = 0;
				ax25->state   = AX25_STATE_3;
				ax25->n2count = 0;
				ax25->dama_slave = dama;	/* bke */
				if (ax25->sk != NULL) {
					ax25->sk->state = TCP_ESTABLISHED;
					/* For WAIT_SABM connections we will produce an accept ready socket here */
					if (!ax25->sk->dead)
						ax25->sk->state_change(ax25->sk);
				}
			}
			break;

		case AX25_DM:
			if (pf) {
				if (ax25->modulus == AX25_MODULUS) {
					ax25_disconnect(ax25, ECONNREFUSED);
				} else {
					ax25->modulus = AX25_MODULUS;
					ax25->window  = ax25_dev_get_value(ax25->device, AX25_VALUES_WINDOW);
				}
			}
			break;

		default:
			if (dama && pf)
				ax25_send_control(ax25, AX25_SABM, AX25_POLLON, AX25_COMMAND);
			break;
	}

	return 0;
}

/*
 *	State machine for state 2, Awaiting Release State.
 *	The handling of the timer(s) is in file ax25_timer.c
 *	Handling of state 0 and connection release is in ax25.c.
 */
static int ax25_state2_machine(ax25_cb *ax25, struct sk_buff *skb, int frametype, int pf, int type)
{
	switch (frametype) {
		case AX25_SABM:
		case AX25_SABME:
			ax25_send_control(ax25, AX25_DM, pf, AX25_RESPONSE);
			if (ax25->dama_slave)
				ax25_send_control(ax25, AX25_DISC, AX25_POLLON, AX25_COMMAND);
			break;

		case AX25_DISC:
			ax25_send_control(ax25, AX25_UA, pf, AX25_RESPONSE);
			if (ax25->dama_slave) ax25_disconnect(ax25, 0);
			break;

		case AX25_DM:
		case AX25_UA:
			if (pf) ax25_disconnect(ax25, 0);
			break;

		case AX25_I:
		case AX25_REJ:
		case AX25_RNR:
		case AX25_RR:
			if (pf) {
				if (ax25->dama_slave)
					ax25_send_control(ax25, AX25_DISC, AX25_POLLON, AX25_COMMAND);
				else
					ax25_send_control(ax25, AX25_DM, AX25_POLLON, AX25_RESPONSE);
			}
			break;

		default:
			break;
	}

	return 0;
}

/*
 *	State machine for state 3, Connected State.
 *	The handling of the timer(s) is in file ax25_timer.c
 *	Handling of state 0 and connection release is in ax25.c.
 */
static int ax25_state3_machine(ax25_cb *ax25, struct sk_buff *skb, int frametype, int ns, int nr, int pf, int type, int dama)
{
	int queued = 0;

	switch (frametype) {
		case AX25_SABM:
		case AX25_SABME:
			if (dama) ax25_dama_on(ax25);
			if (frametype == AX25_SABM) {
				ax25->modulus = AX25_MODULUS;
				ax25->window  = ax25_dev_get_value(ax25->device, AX25_VALUES_WINDOW);
			} else {
				ax25->modulus = AX25_EMODULUS;
				ax25->window  = ax25_dev_get_value(ax25->device, AX25_VALUES_EWINDOW);
			}
			ax25_send_control(ax25, AX25_UA, pf, AX25_RESPONSE);
			ax25->condition = 0x00;
			ax25->t1timer   = 0;
			ax25->t3timer   = ax25->t3;
			ax25->idletimer = ax25->idle;
			ax25->vs        = 0;
			ax25->va        = 0;
			ax25->vr        = 0;
			ax25->dama_slave = dama;
			ax25_requeue_frames(ax25);
			break;

		case AX25_DISC:
			ax25_send_control(ax25, AX25_UA, pf, AX25_RESPONSE);
			ax25_disconnect(ax25, 0);
			break;

		case AX25_DM:
			ax25_disconnect(ax25, ECONNRESET);
			break;

		case AX25_RR:
		case AX25_RNR:
			if (frametype == AX25_RR)
				ax25->condition &= ~AX25_COND_PEER_RX_BUSY;
			else
				ax25->condition |= AX25_COND_PEER_RX_BUSY;
			if (!ax25->dama_slave && type == AX25_COMMAND && pf)
				ax25_enquiry_response(ax25);
			if (ax25_validate_nr(ax25, nr)) {
				ax25_check_iframes_acked(ax25, nr);
				if (ax25->dama_slave && type == AX25_COMMAND && pf)
					dama_enquiry_response(ax25);
			} else {
				ax25_nr_error_recovery(ax25);
				ax25->state = AX25_STATE_1;
			}
			break;

		case AX25_REJ:
			ax25->condition &= ~AX25_COND_PEER_RX_BUSY;
			if (!ax25->dama_slave && type == AX25_COMMAND && pf)
				ax25_enquiry_response(ax25);
			if (ax25_validate_nr(ax25, nr)) {
				ax25_frames_acked(ax25, nr);
				ax25_calculate_rtt(ax25);
				ax25->t1timer = 0;
				ax25->t3timer = ax25->t3;
				ax25_requeue_frames(ax25);
				if (ax25->dama_slave && type == AX25_COMMAND && pf)
					dama_enquiry_response(ax25);
			} else {
				ax25_nr_error_recovery(ax25);
				ax25->state = AX25_STATE_1;
			}
			break;

		case AX25_I:
			if (!ax25_validate_nr(ax25, nr)) {
				ax25_nr_error_recovery(ax25);
				ax25->state = AX25_STATE_1;
				break;
			}
			if (ax25->condition & AX25_COND_PEER_RX_BUSY) {
				ax25_frames_acked(ax25, nr);
			} else {
				ax25_check_iframes_acked(ax25, nr);
			}
			if (ax25->condition & AX25_COND_OWN_RX_BUSY) {
				if (pf)	{
					if (ax25->dama_slave)
						dama_enquiry_response(ax25);
					else
						ax25_enquiry_response(ax25);
				}
				break;
			}
			if (ns == ax25->vr) {
				ax25->vr = (ax25->vr + 1) % ax25->modulus;
				queued = ax25_rx_iframe(ax25, skb);
				if (ax25->condition & AX25_COND_OWN_RX_BUSY)
					ax25->vr = ns;	/* ax25->vr - 1 */
				ax25->condition &= ~AX25_COND_REJECT;
				if (pf) {
					if (ax25->dama_slave)
						dama_enquiry_response(ax25);
					else
						ax25_enquiry_response(ax25);
				} else {
					if (!(ax25->condition & AX25_COND_ACK_PENDING)) {
						ax25->t2timer    = ax25->t2;
						ax25->condition |= AX25_COND_ACK_PENDING;
					}
				}
			} else {
				if (ax25->condition & AX25_COND_REJECT) {
					if (pf) {
						if (ax25->dama_slave)
							dama_enquiry_response(ax25);
						else
							ax25_enquiry_response(ax25);
					}
				} else {
					ax25->condition |= AX25_COND_REJECT;
					if (ax25->dama_slave)
						dama_enquiry_response(ax25);
					else
						ax25_send_control(ax25, AX25_REJ, pf, AX25_RESPONSE);
					ax25->condition &= ~AX25_COND_ACK_PENDING;
				}
			}
			break;

		case AX25_FRMR:
		case AX25_ILLEGAL:
			ax25_establish_data_link(ax25);
			ax25->state = AX25_STATE_1;
			break;

		default:
			break;
	}

	return queued;
}

/*
 *	State machine for state 4, Timer Recovery State.
 *	The handling of the timer(s) is in file ax25_timer.c
 *	Handling of state 0 and connection release is in ax25.c.
 */
static int ax25_state4_machine(ax25_cb *ax25, struct sk_buff *skb, int frametype, int ns, int nr, int pf, int type, int dama)
{
	int queued = 0;

	switch (frametype) {
		case AX25_SABM:
		case AX25_SABME:
			if (dama) ax25_dama_on(ax25);
			if (frametype == AX25_SABM) {
				ax25->modulus = AX25_MODULUS;
				ax25->window  = ax25_dev_get_value(ax25->device, AX25_VALUES_WINDOW);
			} else {
				ax25->modulus = AX25_EMODULUS;
				ax25->window  = ax25_dev_get_value(ax25->device, AX25_VALUES_EWINDOW);
			}
			ax25_send_control(ax25, AX25_UA, pf, AX25_RESPONSE);
			ax25->condition = 0x00;
			ax25->t1timer   = 0;
			ax25->t3timer   = ax25->t3;
			ax25->idletimer = ax25->idle;
			ax25->vs        = 0;
			ax25->va        = 0;
			ax25->vr        = 0;
			ax25->state     = AX25_STATE_3;
			ax25->n2count   = 0;
			ax25->dama_slave = dama;
			ax25_requeue_frames(ax25);
			break;

		case AX25_DISC:
			ax25_send_control(ax25, AX25_UA, pf, AX25_RESPONSE);
			ax25_disconnect(ax25, 0);
			break;

		case AX25_DM:
			ax25_disconnect(ax25, ECONNRESET);
			break;

		case AX25_RR:
		case AX25_RNR:
			if (frametype == AX25_RR)
				ax25->condition &= ~AX25_COND_PEER_RX_BUSY;
			else
				ax25->condition |= AX25_COND_PEER_RX_BUSY;
			if (type == AX25_RESPONSE && pf) {
				ax25->t1timer = 0;
				if (ax25_validate_nr(ax25, nr)) {
					ax25_frames_acked(ax25, nr);
					if (ax25->vs == ax25->va) {
						ax25->t3timer = ax25->t3;
						ax25->n2count = 0;
						ax25->state   = AX25_STATE_3;
					} else {
						ax25_requeue_frames(ax25);
					}
				} else {
					ax25_nr_error_recovery(ax25);
					ax25->state = AX25_STATE_1;
				}
				break;
			}
			if (!ax25->dama_slave && type == AX25_COMMAND && pf)
				ax25_enquiry_response(ax25);
			if (ax25_validate_nr(ax25, nr)) {
				ax25_frames_acked(ax25, nr);
				if (ax25->dama_slave && type == AX25_COMMAND && pf)
					dama_enquiry_response(ax25);
			} else {
				ax25_nr_error_recovery(ax25);
				ax25->state = AX25_STATE_1;
			}
			break;

		case AX25_REJ:
			ax25->condition &= ~AX25_COND_PEER_RX_BUSY;
			if (pf && (type == AX25_RESPONSE || (ax25->dama_slave && type == AX25_COMMAND))) {
				ax25->t1timer = 0;
				if (ax25_validate_nr(ax25, nr)) {
					ax25_frames_acked(ax25, nr);
					if (ax25->vs == ax25->va) {
						ax25->t3timer = ax25->t3;
						ax25->n2count = 0;
						ax25->state   = AX25_STATE_3;
					} else {
						ax25_requeue_frames(ax25);
					}
					if (ax25->dama_slave && type == AX25_COMMAND && pf)
						dama_enquiry_response(ax25);
				} else {
					ax25_nr_error_recovery(ax25);
					ax25->state = AX25_STATE_1;
				}
				break;
			}
			if (!ax25->dama_slave && type == AX25_COMMAND && pf)
				ax25_enquiry_response(ax25);
			if (ax25_validate_nr(ax25, nr)) {
				ax25_frames_acked(ax25, nr);
				ax25_requeue_frames(ax25);
				if (ax25->dama_slave && type == AX25_COMMAND && pf)
					dama_enquiry_response(ax25);
			} else {
				ax25_nr_error_recovery(ax25);
				ax25->state = AX25_STATE_1;
			}
			break;

		case AX25_I:
			if (!ax25_validate_nr(ax25, nr)) {
				ax25_nr_error_recovery(ax25);
				ax25->state = AX25_STATE_1;
				break;
			}
			ax25_frames_acked(ax25, nr);
			if (ax25->condition & AX25_COND_OWN_RX_BUSY) {
				if (pf) {
					if (ax25->dama_slave)
						ax25_enquiry_response(ax25);
					else
						dama_enquiry_response(ax25);
				}
				break;
			}
			if (ns == ax25->vr) {
				ax25->vr = (ax25->vr + 1) % ax25->modulus;
				queued = ax25_rx_iframe(ax25, skb);
				if (ax25->condition & AX25_COND_OWN_RX_BUSY)
					ax25->vr = ns;	/* ax25->vr - 1 */
				ax25->condition &= ~AX25_COND_REJECT;
				if (pf) {
					if (ax25->dama_slave)
						dama_enquiry_response(ax25);
					else
						ax25_enquiry_response(ax25);
				} else {
					if (!(ax25->condition & AX25_COND_ACK_PENDING)) {
						ax25->t2timer    = ax25->t2;
						ax25->condition |= AX25_COND_ACK_PENDING;
					}
				}
			} else {
				if (ax25->condition & AX25_COND_REJECT) {
					if (pf) {
						if (ax25->dama_slave)
							dama_enquiry_response(ax25);
						else
							ax25_enquiry_response(ax25);
					}
				} else {
					ax25->condition |= AX25_COND_REJECT;
					if (ax25->dama_slave)
						dama_enquiry_response(ax25);
					else
						ax25_send_control(ax25, AX25_REJ, pf, AX25_RESPONSE);
					ax25->condition &= ~AX25_COND_ACK_PENDING;
				}
			}
			break;

		case AX25_FRMR:
		case AX25_ILLEGAL:
			ax25_establish_data_link(ax25);
			ax25->state = AX25_STATE_1;
			break;

		default:
			break;
	}

	return queued;
}

/*
 *	Higher level upcall for a LAPB frame
 */
int ax25_process_rx_frame(ax25_cb *ax25, struct sk_buff *skb, int type, int dama)
{
	int queued = 0, frametype, ns, nr, pf;

	if (ax25->state == AX25_STATE_0)
		return 0;

	del_timer(&ax25->timer);

	frametype = ax25_decode(ax25, skb, &ns, &nr, &pf);

	switch (ax25->state) {
		case AX25_STATE_1:
			queued = ax25_state1_machine(ax25, skb, frametype, pf, type, dama);
			break;
		case AX25_STATE_2:
			queued = ax25_state2_machine(ax25, skb, frametype, pf, type);
			break;
		case AX25_STATE_3:
			queued = ax25_state3_machine(ax25, skb, frametype, ns, nr, pf, type, dama);
			break;
		case AX25_STATE_4:
			queued = ax25_state4_machine(ax25, skb, frametype, ns, nr, pf, type, dama);
			break;
	}

	ax25_set_timer(ax25);

	return queued;
}

#endif
