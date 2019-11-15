echo "Enter your choice"
echo "1.Single Producer Multiple Consumers(SPMC)"
echo "2.Multiple Producers Single Consumers(MPSC)"
echo "3.Multiple Producers Multiple Consumers(MPMC)"
#read choice
choice=$1
skip=$2
sim=$3
final_names=(dummy SPMC MPSC MPMC)
orig_names=(Thread1 Thread2 Thread3 Thread4)
gcc -O0 -g sharing.c -lpthread
./make_tracer.sh champsim_tracer
pin -ifeellucky -t obj-intel64/champsim_tracer.so -s $skip -t $sim -- ./a.out $choice
for orig in ${orig_names[*]}
do
	mv ${orig}.trace ${orig}_${final_names[$choice]}.trace
	gzip ${orig}_${final_names[$choice]}.trace
done
