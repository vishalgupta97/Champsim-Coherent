policy=lru #${1}
numcpu=1  #${2}
combination=no-no-no #${3}

IFS='-' # hyphen (-) is set as delimiter
read -ra ADDR <<< "${combination}" # string is read into an array as tokens separated by IFS
l1d_prefetcher=${ADDR[0]}
l2c_prefetcher=${ADDR[1]}
llc_prefetcher=${ADDR[2]}
./build_champsim.sh bimodal ${l1d_prefetcher} ${l2c_prefetcher} ${llc_prefetcher} ${policy} ${numcpu}

