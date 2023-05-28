# A reliable transport protocol over an unreliable channel

This was an assignment I did in the Networks Lab course. Given some libraries that implement a transport layer that is unreliable, we had to build and implement a protocol to establish reliability, similar to TCP.

## Execute
We've created a script `make_both.sh` which could be used to 
make both code and Testing at once, using
$ ./make_both
or running `make` in both individually would also work. 

After make, we need to run
$ ./reliable -w 5 6666 localhost:5555
in code folder, and 
$ ./ref -w 5 5555 localhost:6666 
in Testing folder on another terminal


# Code Explanation

## 1. Sending data
rel_read takes data in from application layer using conn_input,
and then create a packet, buffer it for retransmission, sends it.

The buffer is an array with size = window_size, and the position
of each packet within this array is calculated by taking its 
seqno modullo window_size.

```
sender window
-------------------------
|  |  |  |  |  |  |  |  |
-------------------------
 ^            ^
last-ackno   last-sent
```
last_ackno_rcvd value is the packet receiver is expecting next
total inflight packets: last_sent - last_ack + 1

Whenver the window size is greater than total inflight packets, 
we can send more data through the connection.


## 2. Receiving data
rel_recvpkt is called with the packet that we received. First we 
check whether its corrupted, if not then depending on its lenght
we classify it as ack packet (len=8) or data packet (len>=12).
Data is read from the packet, and stored in a buffer.
when rel_output is called, it checks if the next expected packet
is buffered or not. If it is, then only it delivers it the application
layer and sends ack for that. This ensures proper ordering on 
receiver side.

```
receiver window
-------------------------
|  |  |  |  |  |  |  |  |
-------------------------
 ^            ^
last-acked   last-rcvd
```
next expecting packet with seqno last-acked,
when receive it we'll start pushing data to app layer


## 3) Retransmission
We track timestamps of when the packet was sent in an array,
and when rel_timer is called, it checks if the first unacked packet
has time inflight greater than timeout value, in that case it grabs
the packet from the sender buffer, and retransmits it.

In our implementation we only resent the first unacked packet,
rather than sending all since the receiver could have buffered packets 
with a greater seqno.

rel_timer also checks if we need to close the connection, i.e., we 
read EOF, received EOF and there are no buffered packets on either side.
If so, it calls rel_destroy.
