For purposes of scalability, we wanted to implement multiple FIX servers such that the load from a high number of clients would be distributed to the fix servers. The main concern that comes with this is the face that we would need to aggregate the messages from all the fix servers and send them to the sequencer in the correct order.

The simplest way to address this problem is by using the shared memory system that is provided from the boost library. Using it, we say that the messages that arrive at a single fix server is the first message for that fix server. Boost's lock free queue handles inserting the messages into the queue in the correct order and sent to the sequencer.
  
NOTE: For now, we are using a single machine which contains the server, sequencer and matching engine. Plans are to move the fix servers to their own machines.

## Communication between ports and Sequencer + Matching Engine

There needs to be constant communcation between our ports and the thread/machine which the sequencer resides on. For example, if a message with an incorrect sequence number arrives, then it can be retried or on a succesful match, clients must be notified. There were many design decisions that were considered, but all of them involved shared memory.

### Option 1: Matching Engine back messages to all ports it concenrns

All messages in the exchange have a **single** topic, but multiple ports might want to receive an internal message with the same topic, meaning the matching engine would have to determine all relevant ports which are interested in that message and send it to them directly.
Ater finishing to process a message, this adds a small overhead.

### Option 2: Matching Engine sends messages to topic queues

Instead of sending the messages directly to the ports, the matching engine would send the messgaes to a dictionary structure that has queues for each topic, from which the ports continuously loop over to check if they have messages they are interested in themselves. This leaves the responsibility for most of the work to the ports, letting the matching engine be as efficient as possible.

Option chosen: 2

