After matching orders, the matching engine will need to do some additional tasks.

The first additional task is to update the internal order book. The order book is a data structure that stores the orders that are waiting to be processed by the matching engine. The order book is a binary search tree that is ordered by the price of the orders.

The second additional task is that it needs to notify the client of the success or failure of the order. Using codes like 35=8 (Execution Report) and 35=9 (Order Cancel Reject).

The third additional task is that it needs to update the logs of the trades that were executed. This is important for auditing purposes and for regulatory compliance.