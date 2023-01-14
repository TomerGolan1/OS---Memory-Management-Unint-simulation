
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wait.h>
#include <string.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/types.h>

/*Defines*/
// Waiting Times
#define INTER_MEM_ACCS_T 		250000000	// ns
#define MEM_WR_T 				100			// ns
#define TIME_BETWEEN_SNAPSHOTS 	100000000 	// ns
#define HD_ACCS_T 				1000000	    // ns
#define SIM_T 					4			// seconds

// Probabilities
#define WR_RATE 					0.5
#define HIT_RATE 					0.5

// Memory Sizes
#define USED_SLOTS_TH 	4
#define N 				10

/// Messages
#define BUF_SIZE       128
// Devices Requests
// Message types
// If type is equal to zero, the first message is received -AVOID!
#define PROC_REQ 		1   //Request from all Processes (origin included in message)
#define HD_REQ 			2
#define HD_ACK 			3
#define MMU_ACK1		4   //Ack for Process 1
#define MMU_ACK2		5   //Ack for Process 2
// Sub Request types {for processes}
#define RD_REQ	        0
#define WR_REQ	        1

/// MMU State Machine
// Pages state
#define INVALID		    0
#define VALID			1
#define DIRTY			2
// Answering state for Request
#define MISS			0
#define HIT				1


// Globals
int qid;						// Message queue ID
pid_t pid[3];					// Process IDs
pthread_t tid[3];				// Thread IDs
int pages[N] = {0};				// Pages array - init INVALID
int pagecount = 0;				// Page counter
int FIFO_index = 0;	  // Oldest page index for FIFO clock algorithm
pthread_mutex_t pages_mutex;     // Pages array mutex
pthread_mutex_t count_mutex;	 // Pages counter mutex
pthread_mutex_t FIFO_mutex;		    // FIFO mutex
pthread_mutex_t cv_mutex;          // Condition variable mutex
pthread_cond_t cv_evicter;        // Evicter condition variable

typedef struct msg {
    long    type;            /* Message type - elaborated in define section*/
    int     request;         /* 0 - read, 1 - write */
    int     origin;          /* Process ID - used to distinguish between processes 1 or 2 */
    char    data[BUF_SIZE];  /* Mimics data tranfer */
} msg;


void init();
void processFunc(int);
void HD();
void* MMU_main();
void* Evicter();
void wake_evicter();
void* Printer();
int open_queue(key_t);
void send_message(int, struct msg*);
void read_message(int, struct msg*, long);
void count_add();
void count_sub();
void pages_lock();
void pages_unlock();
void count_lock();
void count_unlock();
void FIFO_lock();
void FIFO_unlock();
void terminate(int);


int main(void) {
	init();
	sleep(SIM_T);
	terminate(0);
    return 0;
}

void init(void) {
    // Function to initialize the system
    // Create unique key via call to ftok() later to be used in msgget()
    // First create the queue
    // Then fork 3 times for total of 4 processes (including main): Process 1, Process 2, HD and MMU  

    time_t t2;                   // For random seed
    srand((unsigned) time(&t2));

    key_t msgkey;
    int i, j;

    // Create queue
	msgkey = ftok(".",'m'); // Create unique key
    if(( qid = open_queue(msgkey)) == -1) {
        msgctl(qid,IPC_RMID,NULL);
        perror("Error Message: in creating queue\n");
        exit(EXIT_FAILURE);
    }

    // Create processes
	for( i=0; i<3; i++){
		pid[i] = fork();
		if (pid[i]<0) {
			perror("Error Message: in fork\n");
            msgctl(qid,IPC_RMID,NULL);
			for( j=i; j>=0; j--) { //Kill all previous processes
				kill(pid[j], SIGKILL);
			}
			exit(EXIT_FAILURE);
		}
        // Route each son to its purpose
		if(pid[i] == 0) {
			switch (i) {
                case 0:     // Process 1
                    processFunc(i);
                    break;
				case 1:     // Process 2
					processFunc(i);
					break;
                case 2:     // HardDisk
                    HD();
                    break;
				default:
					perror("Error Message: invalid switch access after fork\n");
					break;
            // MMU runs on main process (as the father of all)
			}
		}// Father process continues to fork
	}// Father process continues in init

    // Initialize mutexes
    // Every initialiazation must be checked for errors
    // If error occurs, destory all previous initializations and exit
    if (pthread_mutex_init(&pages_mutex, NULL)){
		perror("Error Message: in init page mutex!\n");
        msgctl(qid,IPC_RMID,NULL);
        for( j=0; j<3; j++) { //Kill all processes
            kill(pid[j], SIGKILL);
        }
        pthread_mutex_destroy(&pages_mutex);
		exit(EXIT_FAILURE);
	}
    if (pthread_mutex_init(&count_mutex, NULL)){
		perror("Error Message: in init counter mutex!\n");
        msgctl(qid,IPC_RMID,NULL);
        for( j=0; j<3; j++) { //Kill all processes
            kill(pid[j], SIGKILL);
        }
        pthread_mutex_destroy(&pages_mutex);
        pthread_mutex_destroy(&count_mutex);
		exit(EXIT_FAILURE);
	}
    if (pthread_mutex_init(&FIFO_mutex, NULL)){
		perror("Error Message: in init FIFO mutex!\n");
		msgctl(qid,IPC_RMID,NULL);
        for( j=0; j<3; j++) { //Kill all processes
            kill(pid[j], SIGKILL);
        }
        pthread_mutex_destroy(&pages_mutex);
        pthread_mutex_destroy(&count_mutex);
        pthread_mutex_destroy(&FIFO_mutex);
		exit(EXIT_FAILURE);
	}
    if (pthread_mutex_init(&cv_mutex, NULL)){
		perror("Error Message: in init cv mutex!\n");
		msgctl(qid,IPC_RMID,NULL);
        for( j=0; j<3; j++) { //Kill all processes
            kill(pid[j], SIGKILL);
        }
        pthread_mutex_destroy(&pages_mutex);
        pthread_mutex_destroy(&count_mutex);
        pthread_mutex_destroy(&FIFO_mutex);
        pthread_mutex_destroy(&cv_mutex);
		exit(EXIT_FAILURE);
	}

    // Initialize condition variables
    if (pthread_cond_init(&cv_evicter, NULL)){
        perror("Error Message: in init evicter cv!\n");
        msgctl(qid,IPC_RMID,NULL);
        for( j=0; j<3; j++) { //Kill all processes
            kill(pid[j], SIGKILL);
        }
        pthread_mutex_destroy(&pages_mutex);
        pthread_mutex_destroy(&count_mutex);
        pthread_mutex_destroy(&FIFO_mutex);
        pthread_mutex_destroy(&cv_mutex);
        pthread_cond_destroy(&cv_evicter);
        exit(EXIT_FAILURE);
    }

    // Create MMU threads - as commented before, this process belongs to MMU
    if((pthread_create(&tid[0], NULL, MMU_main, NULL))){
		perror("Error Message: in creating MMU main thread\n");
        msgctl(qid,IPC_RMID,NULL);
        for( j=0; j<3; j++) { //Kill all processes
            kill(pid[j], SIGKILL);
        }
        pthread_mutex_destroy(&pages_mutex);
        pthread_mutex_destroy(&count_mutex);
        pthread_mutex_destroy(&FIFO_mutex);
        pthread_mutex_destroy(&cv_mutex);
        pthread_cond_destroy(&cv_evicter);
        pthread_cancel(tid[0]);
		exit(EXIT_FAILURE);
	}
    if((pthread_create(&tid[1], NULL, Evicter, NULL))){
		perror("Error Message: in creating Evicter thread\n");
		msgctl(qid,IPC_RMID,NULL);
        for( j=0; j<3; j++) { //Kill all processes
            kill(pid[j], SIGKILL);
        }
        pthread_mutex_destroy(&pages_mutex);
        pthread_mutex_destroy(&count_mutex);
        pthread_mutex_destroy(&FIFO_mutex);
        pthread_mutex_destroy(&cv_mutex);
        pthread_cond_destroy(&cv_evicter);
        pthread_cancel(tid[0]);
        pthread_cancel(tid[1]);
		exit(EXIT_FAILURE);
    }
    
	if((pthread_create(&tid[2], NULL, Printer, NULL))){
		perror("Error Message: in creating Printer thread\n");
		msgctl(qid,IPC_RMID,NULL);
        for( j=0; j<3; j++) { //Kill all processes
            kill(pid[j], SIGKILL);
        }
        pthread_mutex_destroy(&pages_mutex);
        pthread_mutex_destroy(&count_mutex);
        pthread_mutex_destroy(&FIFO_mutex);
        pthread_mutex_destroy(&cv_mutex);
        pthread_cond_destroy(&cv_evicter);
        pthread_cancel(tid[0]);
        pthread_cancel(tid[1]);
        pthread_cancel(tid[2]);
		exit(EXIT_FAILURE);
	}
    
}

void* MMU_main() {
 
    int action; // Miss or Hit
    
    msg send;       // Sending message (packet of struct msg)
	msg recvProc;   // Receiving message (packet of struct msg)
    msg recvHD;     // Receiving message for HD ACK

    while(1) {
        // Note the following line reads a process message without discrimation of their origin 
        read_message(qid, &recvProc, PROC_REQ);
        if (pagecount==0) { // If page count is 0, surley a miss
            action = MISS;
        }
        else {              // Otherwise by probability
            action = (rand() % 100 < HIT_RATE*100) ? HIT : MISS;
        }
        
        // Act according to Miss or Hit
        if (action==MISS) {
            // Check Evicter neccessity
            if (pagecount==N) { // Wake Evicter
                wake_evicter();
            }
            // Get page from HD
            send.type = HD_REQ; // Set message type to HD_REQ - HardDisk Request
            send.origin = send.type;    // Set origin to HD_REQ - HD_REQ although not used for it
            strcpy(send.data,"Message is a HD_REQ");
            send_message(qid, &send);
            read_message(qid, &recvHD, HD_ACK); // Wait for HD_ACK (This command is blocking)
            // Now safley update the new page to VALID

            pages_lock();
            pages[(FIFO_index + pagecount) % N] = VALID; // Mark page valid
            pages_unlock();

            // Safe Add to page count
            count_add();        
        }
        else if(action==HIT) {
            //Do nothing - continue to ACK below
        }
        else { // action must be HIT or MISS
            perror("Error Message: invalid action for MMU_main\n");
            terminate(1);
        }

        // Prepare ACK
        send.type = (recvProc.origin==1) ? MMU_ACK1 : MMU_ACK2; // Ack process 1 or 2
        send.origin = send.type; // Not really being used here but for completeness
        strcpy(send.data,"Message is a MMU_ACK 1 or 2");
        // Check RD / WR
        if (recvProc.request==RD_REQ) {
            send_message(qid, &send); //Send MMU_ACK to process
        }
        else if (recvProc.request==WR_REQ) {
            usleep(MEM_WR_T/(double)1000);

            pages_lock();
            pages[(FIFO_index + (rand() % pagecount)) % N] = DIRTY; // Mark page dirty - random from base of FIFO to current page count
            pages_unlock();

            send_message(qid, &send); //Send MMU_ACK to process
        }
        else { // request must be RD or WR
            perror("Error Message: invalid RD/WR request in MMU_main\n");
            terminate(1);
        }
    }
}

void* Evicter() { // This function runs by a thread from MMU process
// Waits to be waken from MMU_main
// Removes pages when called and signals back

    msg send;   //In case of dirty - need to access HD
    msg recv;
    
    send.type = HD_REQ; // Set message type to HD_REQ - HardDisk Request
    send.origin = send.type;    // Set origin to HD_REQ - HD_REQ although not used for it
    strcpy(send.data,"Message is an Evicter dirty HD_REQ access");
    
    while(1) {
        // Get signal from MMU
        if (pthread_mutex_lock(&cv_mutex)) { //safe locking
            perror("Error Message: in locking mutex\n");
            terminate(1);
        }

        if (pthread_cond_wait(&cv_evicter, &cv_mutex)) {
            perror("Error Message: in waiting for evicter\n");
            terminate(1);
        }

        if (pthread_mutex_unlock(&cv_mutex)) { //safe unlocking
            perror("Error Message: in unlocking mutex\n");
            terminate(1);
        }
        // Get here if Evicter was signaled by MMU

        while(N - pagecount < USED_SLOTS_TH) {
            // Check if dirty

            pages_lock();
            if (pages[FIFO_index] == DIRTY) {
                // Write to HD
                send_message(qid, &send);
                read_message(qid, &recv, HD_ACK); // Wait for HD_ACK (This command is blocking)
            }
            pages[FIFO_index] = INVALID; // clear page
            // Advance FIFO
            FIFO_index = (FIFO_index + 1) % N;
            // Remove page
            pages_unlock();
            count_sub();
        }

        // Signal back to MMU
        if (pthread_mutex_lock(&cv_mutex)) { //safe locking
            perror("Error Message: in locking mutex\n");
            terminate(1);
        }

        if (pthread_cond_signal(&cv_evicter)) { // Signal MMU
            perror("Error Message: in signaling mmu\n");
            terminate(1);
        }

        if (pthread_mutex_unlock(&cv_mutex)) { //safe unlocking
            perror("Error Message: in unlocking mutex\n");
            terminate(1);
        }
    }
}

void* Printer() {
    // Function runs by a thread from MMU process
    // Prints the Memory
    int pages_copy[N] = {0};
    int i;

    while(1) {

        pages_lock();
        memcpy(pages_copy, pages, N*sizeof(int)); // Copy pages to pages_copy
        pages_unlock();

        for (i=0; i<N; i++) {
            printf("%d|", i);
            if (pages_copy[i] == INVALID) {
                printf("-\n");
            } else if (pages_copy[i] == VALID) {
                printf("0\n");
            } else if (pages_copy[i] == DIRTY) {
                printf("1\n");
            } else {
                printf("Incorrect page value\n");
            }
        }
        printf("\n\n");
		usleep(TIME_BETWEEN_SNAPSHOTS/(double)1000);
    }
}

void HD() { // This function runs by a process
    // Resembles the HardDisk of the system
    // It is a process that runs in parallel to the MMU
    // It is responsible for reading and writing to the HardDisk

    msg send;
    msg receive;        // Will be of type HD_REQ
    send.type = HD_ACK; // Set message type to HD_ACK - HardDisk Acknowledgement
    send.origin = send.type;    // Set origin to HD_ACK - HardDisk - although not used for it
    strcpy(send.data,"Message is an HD_ACK");

    while(1) {
        read_message(qid, &receive, HD_REQ); // Get only relevant messages
        usleep(HD_ACCS_T/(double)1000);      // Simulate HD access time
        send_message(qid, &send);            // Send HD_ACK
    }
}

void processFunc(int num) {
    // This function resembles the Process 1 & 2 defined in task
    // They access the MMU for read and write requests
    // Input is num=i where i is in {0,1}
    
    time_t t2;                   // For random seed
    srand((unsigned) time(&t2));

    msg send;
    msg receive;

	send.type = PROC_REQ; // Set message type to 1 - Process Request
    send.origin = num+1;    // Set origin to 1 for process 1 or 2 for process 2


    while(1) {
		usleep(INTER_MEM_ACCS_T/(double)1000);
        if (rand() % 100 <= WR_RATE*100) { // Read or Write - randomly chosen
            send.request = WR_REQ;
        }
        else {
            send.request = RD_REQ;
        }
		sprintf(send.data, "Process %d Requested RD/WR 0/1 - %d\n", num, send.request);
        send_message(qid, &send);
		read_message(qid, &receive, MMU_ACK1 + num); //MMU_ACK2 = MMU_ACK1 + 1
	}
}

void wake_evicter() {
    // Signal Evicter to clear pages
    // Called by MMU when pages[N] is full
    // waits until Evicter finishes its job
    // receieves signal from Evicter when it has cleared enough pages
    // returns to MMU to continue
    if (pthread_mutex_lock(&cv_mutex)) { //safe locking
        perror("Error Message: in locking mutex\n");
        terminate(1);
    }

    if (pthread_cond_signal(&cv_evicter)) { // Wake Evicter
        perror("Error Message: in signaling evicter\n");
        terminate(1);
    }

    if (pthread_mutex_unlock(&cv_mutex)) { //safe unlocking
        perror("Error Message: in unlocking mutex\n");
        terminate(1);
    }
    
    // Get signal from Evicter
    if (pthread_mutex_lock(&cv_mutex)) { //safe locking
        perror("Error Message: in locking mutex\n");
        terminate(1);
    }
    
    if (pthread_cond_wait(&cv_evicter, &cv_mutex)) {
        perror("Error Message: in waiting for evicter\n");
        terminate(1);
    }

    if (pthread_mutex_unlock(&cv_mutex)) { //safe unlocking
        perror("Error Message: in unlocking mutex\n");
        terminate(1);
    }
}

int open_queue(key_t keyval) // Open a queue
// function returns queue id on success and -1 on failure
// information was attached in the assignment
{
    int qid_f;
    if((qid_f = msgget( keyval, IPC_CREAT | 0660 )) == -1)
    {
        return(-1);
    }
    return(qid_f);
}

void send_message(int qid_f, struct msg* qbuf)
// function sends a message to the queue with id qid_f
// information was attached in the assignment
{
    int result, length;
    /* The length is essentially the size of the structure minus sizeof long */
    length = sizeof(struct msg) - sizeof(long);        
    if((result = msgsnd( qid_f, qbuf, length, 0)) == -1)
    {
        perror("Error Message: in sending message\n");
        terminate(1);
    }
}

void read_message(int qid_f, struct msg* qbuf, long type)
// function reads a message from the queue with id qid_f
// information was attached in the assignment
{
    int     result, length;
    /* The length is essentially the size of the structure minus sizeof long  */
    length = sizeof(struct msg) - sizeof(long);   
    if((result = msgrcv( qid_f, qbuf, length, type,  0)) == -1)
    {
        perror("Error Message: in receiving message\n");
        terminate(1);
    }
}

void count_add() {
    // Function adds 1 to the page counter
    // Does so safely by locking the mutex

    count_lock();
	pagecount+=1;
    count_unlock();

    if ((pagecount > N) || (pagecount<0)) { // Error - pages in use exceed correct range
        terminate(1);
    }
}

void count_sub() {
    // Function substructs 1 from the page counter
    // Does so safely by locking the mutex

    count_lock();
	pagecount--;
    count_unlock();

    if ((pagecount > N) || (pagecount<0)) { // Error - pages in use exceed correct range
        terminate(1);
    }
}

void count_lock() {
    // Function locks the mutex
    if (pthread_mutex_lock(&count_mutex)) { //safe locking
        perror("Error Message: in locking mutex\n");
        terminate(1);
    }
}

void count_unlock() {
    // Function unlocks the mutex
    if (pthread_mutex_unlock(&count_mutex)) { //safe locking
        perror("Error Message: in unlocking mutex\n");
        terminate(1);
    }
}

void FIFO_lock() {
    // Function locks the mutex
    if (pthread_mutex_lock(&FIFO_mutex)) { //safe locking
        perror("Error Message: in locking mutex\n");
        terminate(1);
    }
}

void FIFO_unlock() {
    // Function unlocks the mutex
    if (pthread_mutex_unlock(&count_mutex)) { //safe locking
    perror("Error Message: in unlocking mutex\n");
    terminate(1);
    }  
}

void pages_lock() {
    // Function locks the mutex
    if (pthread_mutex_lock(&pages_mutex)) { //safe locking
        perror("Error Message: in locking mutex\n");
        terminate(1);
    }
}

void pages_unlock() {
    // Function unlocks the mutex
    if (pthread_mutex_unlock(&pages_mutex)) { //safe locking
        perror("Error Message: in unlocking mutex\n");
        terminate(1);
    }
}

void terminate(int num) {
	// Kill all processes, threads and clear all queues
    // Destroy mutexes and condition variables
    // Input: int: 1 or 0 -> 1 Failure, 0 Success
    int i;

    for( i=0;i<3;i++){
		if (pthread_cancel(tid[i])) {
            perror("Error Message: in destroying threads!\n");
        }
	}

    for( i=0;i<3;i++){
		kill(pid[i], SIGKILL);
	}

    msgctl(qid,IPC_RMID,NULL);

    if (pthread_mutex_destroy(&pages_mutex)) {
        perror("Error in destroying mutex!\n");
    }

    if (pthread_mutex_destroy(&count_mutex)) {
        perror("Error in destroying mutex!\n");
    }

    if (pthread_mutex_destroy(&FIFO_mutex)) {
        perror("Error in destroying mutex!\n");
    }

    if (pthread_mutex_unlock(&cv_mutex)) {
    }

    if (pthread_mutex_destroy(&cv_mutex)){
		perror("Error Message: in destroying cv mutex!\n");
	}

    if (pthread_cond_destroy(&cv_evicter)){
        perror("Error Message: in destroying cv evicter!\n");
    }
    
    if (num == 1) {
        exit(EXIT_FAILURE);
    }
}
