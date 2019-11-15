gcc -O0 sharing.c -lpthread
./make_tracer.sh unique_block
skip=$1
pin -ifeellucky -t obj-intel64/unique_block.so -s $1 -t 1000000 -- ./a.out 1
