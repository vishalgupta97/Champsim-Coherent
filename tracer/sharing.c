
#include <stdio.h>       /* standard I/O routines                 */
#include <pthread.h>     /* pthread functions and data structures */
unsigned long g;
/* function to be executed by the new thread */
void*
do_loop(void* data)
{
    register int i;			/* counter, to print numbers */
    //int j;			/* counter, for delay        */
    //int me = *((int*)data);     /* thread identifying number */
    while (1) {
	//for (j=0; j<1; j++); /* delay loop */
	    
		g = g+1;
        //printf("'%d' - Got '%lu'\n", me, g);
    }

    /* terminate the thread */
    pthread_exit(NULL);
}

/* like any C program, program's execution begins in main */
int main(int argc, char* argv[])
{
    int        thr_id;         /* thread ID for the newly created thread */
    pthread_t  p_thread1,p_thread2;       /* thread's structure                     */
    int        a         = 1;  /* thread 1 identifying number            */
    int        b         = 2;  /* thread 2 identifying number            */
	int c =100;
    /* create a new thread that will execute 'do_loop()' */
    pthread_create(&p_thread1, NULL, do_loop, (void*)&a);
	pthread_create(&p_thread2, NULL, do_loop, (void*)&b);

	pthread_join(p_thread1,NULL);
	pthread_join(p_thread2,NULL);
    
    /* NOT REACHED */
    return 0;
}
