Random Kelvin's notes

commands are requests

fixTask?

What challenges have you encountered, to a production highly used system
What behaviour do you remember, common failure patterns, how did you test/best tests
How were commands sequenced when several sessions or gateways submitted orders concurrently? What did ‘fairness’ mean in practice
What were data structures used, what optimizations were done


A sound path would be:
1. FIX or HTTP gateways convert both requests into protocol-independent commands.
2. The sequencer gives them authoritative AAPL positions 301 and 302.
3. The single AAPL owner applies command 301, then 302.
4. Matching uses only the current book, configuration, and those sequence positions.
5. It emits immutable cancellation, acceptance, resting, rejection, or trade events as defined by adopted rules.
6. Once acceptance is acknowledged, the journal boundary guarantees that the command or completed result is recoverable.
7. Full queues trigger defined backpressure instead of dropping either command.
8. After a crash, the same commands pass through the same matching state machine.
9. Gateways translate resulting events but never edit the order book.
10. Any attempt to make this faster must preserve these results and demonstrate improvement with reproducible measurements.


Bus:
A buffer is temporary memory used to hold data while it moves between components. The bus uses a circular buffer, called a ring buffer.