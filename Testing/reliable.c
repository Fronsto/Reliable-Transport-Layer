
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

#define ACK_PKT_LEN  8
#define DATA_PKT_HEADER_LEN 12
#define DATA_PKT_MAX_PAYLOAD_LEN 500
#define DATA_PKT_MAX_LEN 512

typedef enum boolean {
  FALSE, TRUE
} bool;

enum packet_type {
  DATA_PKT, ACK_PKT, INVALID_PKT
};

typedef struct {
  packet_t *pkts;                // array of packets, buffered for retransmission      
  time_t *pkt_timestamps;        // array of timestamps for each packet

  bool read_eof;                 // true if we have read EOF from the file  

  uint32_t last_ackno_rcvd;      // The last ackno received from the other side of the connection 
  uint32_t last_seqno_sent;      // The sequence number of the last sent packet 
  // sender window
  // -------------------------
  // |  |  |  |  |  |  |  |  |
  // -------------------------
  //  ^            ^
  // last-ackno   last-sent
  // last ack value is the packet receiver is expecting next
  // total inflight packets: last_sent - last_ack + 1
} _sender;

typedef struct {
  packet_t *pkts;                      // array of packets, buffered so as to send to app.layer in order
  bool *valid;                         // array indicating if this position has a packet

  bool rcvd_eof;                       // true if we receiver eof

  uint16_t last_pkt_bytes_outputted;   // if last packet wasn't outputted entirely, track #bytes done

  uint32_t last_seqno_rcvd;            // The sequence number of the last received packet 
  uint32_t last_ackno_sent;            // The last ackno sent to the other side of the connection 
  // receiver window
  // -------------------------
  // |  |  |  |  |  |  |  |  |
  // -------------------------
  //  ^            ^
  // last-acked   last-rcvd
  // next expecting packet with seqno last-acked,
  // when we do we'll start push data to app layer
} _receiver;

struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */

  /* Add your own data fields below this */

  int timeout;            // timeout interval value
  int window_size;        // window size, same for sender and receiver

  _sender sender;
  _receiver receiver;
};

rel_t *rel_list;


/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
	    const struct config_common *cc)
{
  fprintf(stderr, "rel_create is called\n");
  rel_t *r;

  r = xmalloc (sizeof (*r));
  memset (r, 0, sizeof (*r));

  if (!c) {
    c = conn_create (r, ss);
    if (!c) {
      free (r);
      return NULL;
    }
  }

  r->c = c;
  r->next = rel_list;
  r->prev = &rel_list;
  if (rel_list)
    rel_list->prev = &r->next;
  rel_list = r;

  /* Do any other initialization you need here */

  r->window_size = cc->window;
  r->timeout = cc->timeout;

  r->sender.pkts = xmalloc(r->window_size * sizeof(packet_t));
  r->sender.pkt_timestamps = xmalloc(r->window_size * sizeof(time_t));
  r->sender.last_ackno_rcvd = 1; // start at 1, since we're expecting the first packet to have seqno 1
  r->sender.last_seqno_sent = 0; // start at 0, since we haven't sent anything yet
  r->sender.read_eof = FALSE;

  r->receiver.pkts = xmalloc(r->window_size * sizeof(packet_t));
  r->receiver.valid = xmalloc(r->window_size * sizeof(bool));
  r->receiver.rcvd_eof = FALSE;
  r->receiver.last_pkt_bytes_outputted = 0;
  r->receiver.last_ackno_sent = 1; // start at 1, since we're expecting the first packet to have seqno 1
  r->receiver.last_seqno_rcvd = 0; // start at 0, since we haven't received anything yet

  for (int i = 0; i < r->window_size; i++) {
    r->receiver.valid[i] = FALSE; // no packet in receiver pkt list is a valid packet yet
  }
  return r;
}

void
rel_destroy (rel_t *r)
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);

  /* Free any other allocated memory here */

  free(r->sender.pkts);
  free(r->sender.pkt_timestamps);
  free(r->receiver.pkts);
  free(r->receiver.valid);

  free(r);
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void
rel_demux (const struct config_common *cc,
	   const struct sockaddr_storage *ss,
	   packet_t *pkt, size_t len)
{
}

bool should_close_conn(rel_t *r) {
  return (
    ( r->receiver.rcvd_eof == TRUE ) && // received EOF
    ( r->receiver.last_ackno_sent > r->receiver.last_seqno_rcvd ) && // no buffered pkts
    ( r->sender.read_eof == TRUE ) && // read EOF
    ( r->sender.last_ackno_rcvd == 1 + r->sender.last_seqno_sent ) // no inflight packets
  );
}

enum packet_type get_pkt_type(packet_t *pkt, size_t n) {
  if (n < ACK_PKT_LEN || n > DATA_PKT_MAX_LEN || (n > ACK_PKT_LEN && n < DATA_PKT_HEADER_LEN)) {
    fprintf(stderr, "%d: invalid packet length: %zu\n", getpid(), n);
    return INVALID_PKT;
  }

  int pkt_len = ntohs(pkt->len);
  if(pkt_len==0 || n < pkt_len){ // check if packet got truncated
    fprintf(stderr, "%d: invalid lenght field: %d, n=%zu\n", getpid(), pkt_len,n);
    return INVALID_PKT;
  }

  uint16_t cksum_val = pkt->cksum; pkt->cksum = 0;
  uint16_t cksum_computed = cksum((void *)pkt, pkt_len);
  pkt->cksum = cksum_val; 
  if (cksum_val != cksum_computed) {
    fprintf(stderr, "%d: invalid checksum: %04x\n", getpid(), pkt->cksum);
    return INVALID_PKT;
  }

  if (pkt_len == ACK_PKT_LEN) return ACK_PKT;
  else return DATA_PKT;
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
  enum packet_type pkt_type = get_pkt_type(pkt, n);
  if(pkt_type == INVALID_PKT) {
    if(n > 8){
      // send ack for the last packet we received
      packet_t ack_pkt;
      ack_pkt.len = ACK_PKT_LEN; ack_pkt.len = htons(ack_pkt.len);
      ack_pkt.ackno = r->receiver.last_ackno_sent ; ack_pkt.ackno = htonl(ack_pkt.ackno);
      ack_pkt.cksum = 0; ack_pkt.cksum = cksum((void *)&ack_pkt, ACK_PKT_LEN);
      if( conn_sendpkt(r->c, &ack_pkt, ACK_PKT_LEN) <= 0 ) {
        fprintf(stderr, "%d: error calling conn_sendpkt\n", getpid());
      } else {
        print_pkt(&ack_pkt, "SEND ACK-dropped pkt", ACK_PKT_LEN);
      }
    }
    return; // discard invalid packet
  }

  print_pkt(pkt, "Received ", ntohs(pkt->len));

  // taking care of endianess, converting from network to host
  pkt->len = ntohs(pkt->len);
  pkt->seqno = ntohl(pkt->seqno);
  pkt->ackno = ntohl(pkt->ackno);


  if(pkt_type == ACK_PKT){
    // first check if ackno is within window and greater than already recieved
    if (pkt->ackno <= r->sender.last_ackno_rcvd) {
      fprintf(stderr, "%d: ignoring ackno smaller than already recvd ack\n", getpid());
      return;
    } else if (pkt->ackno > r->sender.last_seqno_sent + 1) {
      fprintf(stderr, "%d: invalid ackno %u\n", getpid(), pkt->ackno);
      return;
    }

    r->sender.last_ackno_rcvd = pkt->ackno; // update ackno
    rel_read(r); // try to read more data and send it

  } else if (pkt_type == DATA_PKT) {

    // condition checks to see if we need to drop this packet
    bool drop_packet = TRUE; 

    if ( pkt->seqno < r->receiver.last_ackno_sent || pkt->seqno >= r->receiver.last_ackno_sent + r->window_size ) 
      fprintf(stderr, "%d: dropping data pkt, outsize window seqno=%u\n", getpid(), pkt->seqno);

    else if (r->receiver.valid[pkt->seqno % r->window_size]) 
      fprintf(stderr, "%d: ignoring duplicate data packet seqno=%u\n", getpid(), pkt->seqno);

    else if (r->receiver.rcvd_eof == TRUE) 
      fprintf(stderr, "%d: ignoring data packet, already received EOF\n", getpid());

    else if (pkt->len == DATA_PKT_HEADER_LEN) {
      if (r->receiver.last_ackno_sent < r->receiver.last_seqno_rcvd) {
        fprintf(stderr, "%d: ignoring EOF, waiting for seqno=%u\n", getpid(), r->receiver.last_ackno_sent);
      } else {
        fprintf(stderr, "%d: received EOF\n", getpid());
        // handling EOF : don't drop the packet, send ACK and set rcvd_eof to true
        r->receiver.rcvd_eof = TRUE;
        conn_output(r->c, NULL, 0); // output EOF to application
        packet_t ack_pkt;
        ack_pkt.len = ACK_PKT_LEN; ack_pkt.len = htons(ack_pkt.len);
        ack_pkt.ackno = 1 + r->receiver.last_ackno_sent ; ack_pkt.ackno = htonl(ack_pkt.ackno);
        ack_pkt.cksum = 0; ack_pkt.cksum = cksum((void *)&ack_pkt, ACK_PKT_LEN);
        if( conn_sendpkt(r->c, &ack_pkt, ACK_PKT_LEN) <= 0 ) { // send ACK
          fprintf(stderr, "error occured calling conn_sendpkt" );
        } else {
          print_pkt(&ack_pkt, "SEND ACK for EOF", ACK_PKT_LEN);
          r->receiver.last_ackno_sent++;
        }
        return;
      }
    } 
    else {
      drop_packet = FALSE;
    }

    if(drop_packet == TRUE) { // send ACK for last packet recieved
      packet_t ack_pkt;
      ack_pkt.len = ACK_PKT_LEN; ack_pkt.len = htons(ack_pkt.len);
      ack_pkt.ackno = r->receiver.last_ackno_sent ; ack_pkt.ackno = htonl(ack_pkt.ackno);
      ack_pkt.cksum = 0; ack_pkt.cksum = cksum((void *)&ack_pkt, ACK_PKT_LEN);
      if( conn_sendpkt(r->c, &ack_pkt, ACK_PKT_LEN) <= 0 ) {
        fprintf(stderr, "%d: error calling conn_sendpkt\n", getpid());
      } else {
        print_pkt(&ack_pkt, "SEND ACK-dropped pkt", ACK_PKT_LEN);
      }
      return;
    }
    
    // add the packet to buffer
    r->receiver.pkts[pkt->seqno % r->window_size] = *pkt;
    r->receiver.valid[pkt->seqno % r->window_size] = TRUE;
    if (pkt->seqno > r->receiver.last_seqno_rcvd) {
      r->receiver.last_seqno_rcvd = pkt->seqno;
    }

    // call output to provide the data to application layer
    rel_output(r);
  } 

}


void
rel_read (rel_t *s)
{
  if (s->sender.read_eof == TRUE) {
    fprintf(stderr, "%d: rel_read - already read EOF\n", getpid());
    return;
  }
  int slots_available = s->window_size - (s->sender.last_seqno_sent - s->sender.last_ackno_rcvd + 1);

  while(slots_available-->0){
    // get the packet to send
    packet_t pkt;
    
    int bytes_read = conn_input(s->c, pkt.data, DATA_PKT_MAX_PAYLOAD_LEN); // read data from application layer
    fprintf(stderr, "%d: READ len = %d\n", getpid(), bytes_read);

    if (bytes_read < 0) {
      s->sender.read_eof = TRUE;
      pkt.len = 0;
    } else if (bytes_read == 0) { 
      return; // no more data to read
    } else {
      pkt.len = bytes_read;
    }

    pkt.len += DATA_PKT_HEADER_LEN; pkt.len = htons(pkt.len);
    pkt.seqno = s->sender.last_seqno_sent + 1; pkt.seqno = htonl(pkt.seqno);
    pkt.ackno = s->receiver.last_ackno_sent; pkt.ackno = htonl(pkt.ackno);
    pkt.cksum = 0; pkt.cksum = cksum((void*)&pkt, ntohs(pkt.len));

    // SEND PACKET
    s->sender.pkts[(s->sender.last_seqno_sent + 1) % s->window_size] = pkt; // save pkt to buffer
    s->sender.pkt_timestamps[(s->sender.last_seqno_sent + 1) % s->window_size] = time(NULL);  // save timestamp
    if ( conn_sendpkt(s->c, &pkt, ntohs(pkt.len)) <= 0) { // send
      fprintf(stderr, "%d: rel_read - conn_sendpkt failed\n", getpid());
    } else {
      s->sender.last_seqno_sent++; // update last_seqno_sent
      print_pkt(&pkt, "SEND DATA", ntohs(pkt.len));
    }

    if(s->sender.read_eof == TRUE){
      if(should_close_conn(s)) {
        rel_destroy(s);
      }
      return; // nothing more to read
    } 
  }
}

uint16_t output_pkt(rel_t *r, packet_t *pkt, uint16_t start, uint16_t payload_len) {
  uint16_t bufspace = conn_bufspace(r->c);
  if (bufspace <= 0) {
    fprintf(stderr, "%d: no bufspace available\n", getpid());
    return 0;
  }

  uint16_t bytes_to_output = ( bufspace > payload_len - start ) ? payload_len - start : bufspace;
  char* buf = xmalloc( bytes_to_output * sizeof(char) );
  memcpy(buf, pkt->data + start, bytes_to_output);

  int bytes_outputted = conn_output(r->c, buf, bytes_to_output); 
  assert(bytes_outputted != 0);  // shouldn't be 0 since we checked bufspace
  free(buf);

  return bytes_outputted;
}

void
rel_output (rel_t *r)
{
  uint32_t idx = r->receiver.last_ackno_sent % r->window_size;
  uint16_t num_pkts = 0;

  // while we have buffered packets we can output in order
  while (r->receiver.valid[idx] == TRUE) { 
    packet_t *pkt = &r->receiver.pkts[idx];
    uint16_t payload_len = pkt->len - DATA_PKT_HEADER_LEN;

    // output to application layer, using this function to handle bufspace
    uint16_t bytes_outputted = output_pkt(r, pkt, r->receiver.last_pkt_bytes_outputted, payload_len);
    if (bytes_outputted < 0) {
      fprintf(stderr, "%d: error occured calling conn_output\n", getpid());
      rel_destroy(r);
      return;
    }

    uint16_t bytes_left = payload_len - bytes_outputted - r->receiver.last_pkt_bytes_outputted;
    if (bytes_left > 0) { // not enought buffer space available - we didn't output the whole packet
      r->receiver.last_pkt_bytes_outputted += bytes_outputted;
      break; // when the buffer has space we'll output the rest of the packet
    }
  
    r->receiver.last_pkt_bytes_outputted = 0;
    r->receiver.valid[idx] = FALSE;

    num_pkts += 1;
    idx = (idx + 1) % r->window_size; // move to the next index
  }

  if (num_pkts > 0) { // Send ACK
    packet_t ack_pkt;
    ack_pkt.len = ACK_PKT_LEN; ack_pkt.len = htons(ack_pkt.len);
    ack_pkt.ackno = r->receiver.last_ackno_sent + num_pkts; ack_pkt.ackno = htonl(ack_pkt.ackno);
    ack_pkt.cksum = 0; ack_pkt.cksum = cksum((void *)&ack_pkt, ACK_PKT_LEN);
    if( conn_sendpkt(r->c, &ack_pkt, ACK_PKT_LEN) <= 0 ) {
      fprintf(stderr, "%d: error calling conn_sendpkt", getpid());
    } else {
      r->receiver.last_ackno_sent += num_pkts;
      print_pkt(&ack_pkt, "SEND ACK", ACK_PKT_LEN);
    }
  }
}

void
rel_timer ()
{
  /* Retransmit any packets that need to be retransmitted */
  rel_t *r = rel_list;

  if(should_close_conn(r)) { // checking if all conditions meet
    rel_destroy(r);
  } 
  
  int inflight_pkts = r->sender.last_seqno_sent - r->sender.last_ackno_rcvd ;
  if( inflight_pkts < 0) {
    return; // no inflight packets
  } 

  // difftime returns seconds, so multiply by 1000 to get milliseconds
  int elapsed_millis = 1000 * difftime(time(NULL), r->sender.pkt_timestamps[r->sender.last_ackno_rcvd % r->window_size]);

  // check if time difference is greater than timeout
  if(elapsed_millis < r->timeout) {
    return;
  }
  // retransmit
  packet_t pkt = r->sender.pkts[r->sender.last_ackno_rcvd % r->window_size];
  r->sender.pkt_timestamps[r->sender.last_ackno_rcvd % r->window_size] = time(NULL);
  if ( conn_sendpkt(r->c, &pkt, sizeof(pkt)) <= 0) { // resend
    fprintf(stderr, "%d: rel_timer - conn_sendpkt failed\n", getpid());
  } else {
    print_pkt(&pkt, "Resend data pkt ", ntohs(pkt.len));
  }
  if(should_close_conn(r)) {
    rel_destroy(r);
  }
}
