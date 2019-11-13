
#include <stdio.h>       /* standard I/O routines                 */
#include <pthread.h>     /* pthread functions and data structures */
#define ITERATIONS 1000000
unsigned long g=5;
/* function to be executed by the new thread */
void*
producer(void* data)
{
    register int i;			/* counter, to print numbers */
    int j;			/* counter, for delay        */
    int me = *((int*)data);     /* thread identifying number */
    while (j++ < ITERATIONS) {
	//for (j=0; j<1; j++); /* delay loop */
	    
		g = g+1;
        //printf("Producer:'%d' - Got '%lu'\n", me, g);
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
    int me = *((int*)data);     /* thread identifying number */
    while (j++ < ITERATIONS) {
    //for (j=0; j<1; j++); /* delay loop */

        i = g+1;
        printf("Consumer:'%d' - Got '%lu'\n", me, g);
    }

    /* terminate the thread */
    //pthread_exit(NULL);
}


/* like any C program, program's execution begins in main */
int main(int argc, char* argv[])
{
    int        thr_id,choice;         /* thread ID for the newly created thread */
    pthread_t  p_thread1,p_threadc[4],p_threadp[4];       /* thread's structure                     */
    int        a         = 1;  /* thread identifying number            */
    int        id[4]         = {1,2,3,4};  /* thread identifying number            */
	register int i;
	printf("Enter your choice\n");
	printf("1.Multiple Consumer Single Producer\n");
	printf("2.Single Consumer Multiple Producers\n");
	printf("3.Multiple consumers Multiple Producers\n");
	scanf("%d",&choice);
	switch(choice)
	{
		case 1:
			/* create new threads which will execute 'consumer()' */
			for(i=0;i<4;i++)
				pthread_create(&p_threadc[i], NULL, consumer, (void*)&id[i]);

			/* create a new thread that will execute 'producer()' */
			pthread_create(&p_thread1, NULL, producer, (void*)&a);

			for(i=0;i<4;i++)
				pthread_join(p_threadc[i],NULL);

			pthread_join(p_thread1,NULL);
			break;
		case 2:
            /* create new threads which will execute 'producer()' */
            for(i=0;i<4;i++)
                pthread_create(&p_threadp[i], NULL, producer, (void*)&id[i]);

            /* create a new thread that will execute 'consumer()' */
            pthread_create(&p_thread1, NULL, consumer, (void*)&a);

            for(i=0;i<4;i++)
                pthread_join(p_threadp[i],NULL);

            pthread_join(p_thread1,NULL);
            break;

        case 3:
            /* create new threads which will execute 'producer()' */
            for(i=0;i<4;i++)
                pthread_create(&p_threadp[i], NULL, producer, (void*)&id[i]);

            /* create new threads which will execute 'consumer()' */
			for(i=0;i<4;i++)
            	pthread_create(&p_threadc[i], NULL, consumer, (void*)&id[i]);

            for(i=0;i<4;i++)
                pthread_join(p_threadp[i],NULL);

            for(i=0;i<4;i++)
            	pthread_join(p_threadc[i],NULL);
            break;


	}
    
    /* NOT REACHED */
    return 0;
}
