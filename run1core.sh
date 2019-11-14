./build.sh
i=1
traces=($(ls /home/sam/Thesis/Champsim-EvictionTest/DPC-traces))
sim=10
for trace in ${traces[*]}
do
	if [ $i == 16 ]; then
		./run_champsim.sh bimodal-no-no-no-lru-1core 0 ${sim} ${trace}
		i=1
	else
        ./run_champsim.sh bimodal-no-no-no-lru-1core 0 ${sim} ${trace} &
	fi
	((i++))
done

