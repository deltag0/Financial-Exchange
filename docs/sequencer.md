The goal of the sequencer and the shared memory system was to have a system that could process as many messages as possible concurrently without blocking.

There were 2 options considered for the design:

1. 
One of the observations, we can make is that offers for different tickers are independent of each other. Therefore, we would only need to block processing messages from the sequencer, if the sequencer was about to process a message for a ticker that is currently being processed by another thread. Otherwise, we wouldn't need any blocking operations. This means that we could have multiple threads for the matching engine without being worried about race conditions between threads. However we would need to have a shared state for the sequencer that would need to be protected by a lock, including overhead.
  
2. 
Option 1 would require that every matching engine thread looks up a shared state which would have to be memory safe, and protected by a lock, including overhead. Option 2 works with multiple sequencers threads, where tickers are hashed and assigned to their own sequencer and matching engine. This way, we can preserve cache locality, avoid the overhead of shared state.

We chose to proceed with option 2 because its the option that would probably scale the most. Furthermore, if the system would ever have a lot of clients trying to trade the same ticker at once, option 1 would cause more latency in important moments.