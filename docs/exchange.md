Currently there will be threads for:
- QuickFIX receiving FIX messages
- FIX ports handling messages
- Sequencer 
- Matching engine

Each port has private shared queues:
- fixMessageQueue (between QuickFix and FIX port)
- internalMessageQueue (between ME and FIX port)

Sequencer 2 shared queue:
- mq_shards (between FIX port and Sequencer)
- Between (ME and Sequencer)
