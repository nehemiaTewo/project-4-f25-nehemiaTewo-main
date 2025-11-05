# Project-4-F25
# Project 4: Restaurant Management Simulation with Threads and Shared Memory

This project requires implementing a multithreaded restaurant management simulation using POSIX threads, mutexes, condition variables, and semaphores in C. The system models customers, waiters, and chefs that interact concurrently. The goal is to achieve correct synchronization and coordination among these entities.

Conceptually, this project is like Project 3. There are a few differences. One major difference is the implementation of the actors in the simulation as threads. A second major difference is the use of shared memory queues for exchange of information among the resources. Shared memory requires the use of mutual exclusion and synchronization to safely exchange information among the threads. Another change is the use of a queue for arriving customers to wait until a table is available. In this problem there are multiple waiters. Waiters have additional tasks of seating customers as tables become available and delivering meals. Condition variables are used to synchronize and trigger activities in the system.

# Learning Objectives

1.         Understand and apply mutual exclusion and condition synchronization.

2.         Use Pthread mutexes, condition variables, and semaphores.

3.         Design producer-consumer queues safely accessed by multiple threads.

4.         Avoid race conditions, deadlocks, and starvation.

# System Roles

- Customers: Arrive randomly, wait to be seated, place orders, eat, and leave.
- Waiters: Seat customers, take orders, and deliver meals.
- Chefs: Prepare orders and place finished dishes into a ready queue.

# Architecture
- Customers arrive and enqueue themselves into a bounded waiting queue (fixed-size circular array).
- Waiters dequeue customers from the waiting queue, seat them (acquiring a table semaphore), and take orders.
- Orders are pushed to the shared order queue for chefs.
- Chefs prepare orders and push them to a completed queue.
- Waiters deliver completed meals to customers using per-customer condition variables.

# Synchronization
- Waiting queue: protected by mutex and condition variables not_empty / not_full.
- Tables: semaphore limits number of customers seated at once.
- Orders: mutex + condition variable (order_queue).
- Delivery: per-customer mutex + condition variable.

# Shared Resources
- tables_sem: Counting semaphore controlling available tables.
- waiting_count: Shared counter for waiting customers (protected by mutex).
- order_queue: Queue of orders waiting for chefs (protected by mutex/condition variable).
- completed_queue: Queue of finished dishes ready for delivery to customers.
- Customer.cond_meal_ready: Per-customer condition variable for meal delivery.

# Synchronization Patterns
- Tables: Controlled via semaphores.
- Order and Completed Queues: Classic producer/consumer queues protected by mutex and condition variable.
- Meal Delivery: Waiters signal customer condition variable when meal is ready (chef completed).

# Project Deliverables
- Source code implementing restaurant.c and any headers.
- Makefile compiling with -pthread -Wall.
- README.md describing design and testing.
- 2-3 page report discussing concurrency issues, results and metrics used in solution

## Week 1 Deliverables
- Data structures for customers, tables and orders
- Waiting queue , Order queue and completed queue functions for queue/dequeue
- Main thread, customer thread, waiter thread

## Week 2 Deliverables and Project completion
- Event logging (use code from Project 3)
- Chef thread
- Waiting queue semaphore, locks/mutexes and condition variable integration
- Testing with different scenarios

# Testing Scenarios
- Low Load: Fewer customers than tables.
- High Load: Many more customers than tables.
- Multiple chefs/waiters: Demonstrate concurrency.
- Stress Test: 100+ customers, random arrivals.

# Grading Rubric
- Correct synchronization and concurrency 40
- Queue implementation correctness 20
- Waiter/Chef/Customer coordination 20
- Code style and documentation10
- Testing and discussion10

# Testing / Reporting
- Terminates after TOTAL_CUSTOMERS are served.
- All threads exit gracefully.
- - Insert prints of event occurrences with thread IDs and time of event.
- Create log file of the simulation activity
- Test with random delays to reveal timing bugs.

## Adjustable parameters for testing:
- TOTAL_CUSTOMERS
- NUM_TABLES
- NUM_WAITERS
- NUM_CHEFS
- WAITING_CAPACITY

## Grading Notes:  

No Demo -30%  
Does not compile and run (or crashes) -15%  
Student should identify with code comments for each of the graded items -5%  
5% deduction for missed 'deliverable deadline'  
