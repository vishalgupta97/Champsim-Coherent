./build.sh
trace=600.perlbench_s-1273B.champsimtrace.xz 
#trace=605.mcf_s-1554B.champsimtrace.xz #
sim=10
./run_champsim.sh bimodal-no-no-no-lru-1core 0 ${sim} ${trace} 
vim results_${sim}M/${trace}-bimodal-no-no-no-lru-1core.txt
