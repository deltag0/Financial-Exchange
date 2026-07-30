> **Historical note:** This file is preserved for background only. It is not authoritative and may
> contradict `../exchange-rules.md`, `../architecture.md`, or `../implementation-status.md`.

Ok,

First the sequence numbers per topic

So the sequence numbers are literally per topic only (and the topic is basically just the port), so then they are monotonically increasing, and if we get a bad topic number, we just retry
and after retrying if it works, we're chill, if not (like a late cancel, it's bad)

Another thing is the communication between the ports and the sequencer + matching engine

So:

sequencer + matching engine have SHARED shared-queues with each port

Ports have PRIVATE shared queues where message from internal communicaiton (usually from the ME are sent) and where the quickFIX sends (1 shared queue for each)


check for the publisher ID when receiving message back from the matching engine
