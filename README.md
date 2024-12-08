# Design choices
## Client and server design
I kept the client and server implementations pretty much the same as project0. At the beginning of their main loops the three way handshake is conducted. Following that, they listen for packets and then send packets.
## Modeling the sent & received packet buffers
I used a circular buffer (implemented via array) to represent the sent and received buffers. This was the simplest way I could think of to do it in C- you can easily insert and remove a packet. Searching for packets is inefficient (when looking for packets to print out it's O(n^2)) but I figured this trade off was worth it.

# Problems & Solutions
1. I had an issue where the client would keep retransmitting packets even though it received the proper ack. I realized this was because packets were not being removed from the send buffer upon receival of an ack and this was because I was setting the ack flag as 0b00000001 instead of 0b00000010 lol.
2. Packets got retransmitted when multiple packets without acks were getting received because those packets were viewed as duplicate transmission of ack=0. I fixed this by only checking for duplicate acks if the ack flag is set.
3. The client/server would continue sending packets past the 20 window limit. Upon closer examination I realized that random packets were getting removed from the send buffer because of a bug where the receiving packets' ack numbers were not being converted from big to little endian. (Endian-ness is accounted for everywhere else but I forgot to account for it here)
4. So apparently endian-ness was not accounted for everywhere else... I forgot to change the length field to little endian when passing it into the sendto function so upon retransmission, packets would not actually be transmitted. The problem is always endian-ness. I am going to scream.