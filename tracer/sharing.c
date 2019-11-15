
#include <stdio.h>       /* standard I/O routines                 */
#include <pthread.h>     /* pthread functions and data structures */
#define ITERATIONS 1000000000
#define WAIT_CYCLES 10000 // Wait cycles for producer
#define TOTAL_THREADS 4
unsigned long g=5;
/* function to be executed by the new thread */
void*
producer(void* data)
{
    register int j;			/* counter, for delay        */
    register int me = *((int*)data);     /* thread identifying number */
    while (1) {
	for(j=0;j< (WAIT_CYCLES/4)*me;j++)
		asm("nop");
        //printf("Producer:'%d' - Got '%lu'\n", me, g);
		g = g+1;
    }

    /* terminate the thread */
    pthread_exit(NULL);
}

/* function to be executed by the new thread */
void*
consumer(void* data)
{
    register int i;         /* counter, to print numbers */
    int j=0;            /* counter, for delay        */
    register int me = *((int*)data);     /* thread identifying number */
    while (1) {
        i = g+1;
        //printf("Consumer:'%d' - Got '%lu'\n", me, g);
    }

    /* terminate the thread */
    pthread_exit(NULL);
}


/* like any C program, program's execution begins in main */
int main(int argc, char* argv[])
{
    int        thr_id,choice;         /* thread ID for the newly created thread */
    pthread_t  p_thread1,p_threads[TOTAL_THREADS];       /* thread's structure                     */
    int        a         = 1;  /* thread identifying number            */
    int        id[4]         = {1,2,3,4},i;  /* thread identifying number            */
	/*printf("Enter your choice\n");
	printf("1.Multiple Consumer Single Producer\n");
	printf("2.Single Consumer Multiple Producers\n");
	printf("3.Multiple consumers Multiple Producers\n");
	scanf("%d",&choice);*/
	choice = atoi(argv[1]);
	switch(choice)
	{
		case 1:

			/* create a new thread that will execute 'producer()' */
			pthread_create(&p_thread1, NULL, producer, (void*)&a);
			/* create new threads which will execute 'consumer()' */
			for(i=0;i<TOTAL_THREADS-1;i++)
				pthread_create(&p_threads[i], NULL, consumer, (void*)&id[i]);

			for(i=0;i<4;i++)
				pthread_join(p_threads[i],NULL);

			pthread_join(p_thread1,NULL);
			break;
		case 2:
            /* create new threads which will execute 'producer()' */
            for(i=0;i<TOTAL_THREADS-1;i++)
                pthread_create(&p_threads[i], NULL, producer, (void*)&id[i]);

            /* create a new thread that will execute 'consumer()' */
            pthread_create(&p_thread1, NULL, consumer, (void*)&a);

            for(i=0;i<4;i++)
                pthread_join(p_threads[i],NULL);

            pthread_join(p_thread1,NULL);
            break;

        case 3:
            /* create new threads which will execute 'producer()' and consumer() */
            for(i=0;i<TOTAL_THREADS/2;i++)
			{
                pthread_create(&p_threads[i], NULL, producer, (void*)&id[i]);
			}

            for(i=0;i<TOTAL_THREADS/2;i++)
                pthread_create(&p_threads[i+TOTAL_THREADS/2], NULL, consumer, (void*)&id[i]);

            for(i=0;i<TOTAL_THREADS;i++)
                pthread_join(p_threads[i],NULL);

            break;


	}
    
    /* NOT REACHED */
    return 0;
}
