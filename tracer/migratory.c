
#include <stdio.h>       /* standard I/O routines                 */
#include <pthread.h>     /* pthread functions and data structures */
unsigned long g;

pthread_mutex_t a_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t b_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t c_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t d_mutex = PTHREAD_MUTEX_INITIALIZER;

/* function to be executed by the new thread */
void*
do_loop(void* data)
{
    //register int i;			/* counter, to print numbers */
    //int j;			/* counter, for delay        */
    int me = *((int*)data),rc;     /* thread identifying number */
    while (1) 
	{
		//for (int j=0; j<100000000; j++); /* delay loop */
		if(me == 1)
			rc = pthread_mutex_lock(&a_mutex);
		else if(me == 2)
			rc = pthread_mutex_lock(&b_mutex);
        else if(me == 3)
            rc = pthread_mutex_lock(&c_mutex);
        else if(me == 4)
            rc = pthread_mutex_lock(&d_mutex);


		if (rc) 
		{ /* an error has occurred */
			perror("pthread_mutex_lock");
			pthread_exit(NULL);
		}		    
			g = g+1;
			//printf("'%d' - Got '%lu'\n", me, g);
		if(me == 1)
			rc = pthread_mutex_unlock(&b_mutex);
		else if(me == 2)
            rc = pthread_mutex_unlock(&c_mutex);
		else if(me == 3)
            rc = pthread_mutex_unlock(&d_mutex);
		else if(me == 4)
            rc = pthread_mutex_unlock(&a_mutex);

		if (rc) 
		{
    		perror("pthread_mutex_unlock");
    		pthread_exit(NULL);
		}

	}

		/* terminate the thread */
		pthread_exit(NULL);
}

/* like any C program, program's execution begins in main */
int main(int argc, char* argv[])
{
    int        thr_id;         /* thread ID for the newly created thread */
    pthread_t  p_thread1,p_thread2,p_thread3,p_thread4;       /* thread's structure                     */
    int        a         = 1;  /* thread 1 identifying number            */
    int        b         = 2;  /* thread 2 identifying number            */
	int c =3,d=4;

	pthread_mutex_lock(&b_mutex);
	pthread_mutex_lock(&c_mutex);
	pthread_mutex_lock(&d_mutex);

    /* create a new thread that will execute 'do_loop()' */
    pthread_create(&p_thread1, NULL, do_loop, (void*)&a);
	pthread_create(&p_thread2, NULL, do_loop, (void*)&b);
    pthread_create(&p_thread3, NULL, do_loop, (void*)&c);
    pthread_create(&p_thread4, NULL, do_loop, (void*)&d);


	pthread_join(p_thread1,NULL);
	pthread_join(p_thread2,NULL);
    pthread_join(p_thread3,NULL);
    pthread_join(p_thread4,NULL);

    
    /* NOT REACHED */
    return 0;
}
