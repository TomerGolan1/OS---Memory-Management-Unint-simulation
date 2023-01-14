MMU simulation

The program has 4 processes:

2 processes which simulate memories accesses by 2 defferent processes.
1 process which simulate the delay of an access to the hard-drive.

1 process to simulate the MMU action. It has 3 threads:
The main thread which is the MMU.
Evicter Thread to release frames in the RAM,
and one for randoms printings of the RAM situation with description of the frames. 