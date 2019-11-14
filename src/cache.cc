#include "cache.h"
#include "set.h"
#include "ooo_cpu.h"
#include "uncore.h"

uint64_t l2pf_access = 0;

bool CACHE::make_inclusive(CACHE &cache,uint64_t address)
{
	if(cache.MSHR.occupancy != 0)
	{
		for (uint32_t i=0; i<cache.MSHR.SIZE; i++)
			if (cache.MSHR.entry[i].address == address)
					return false;
	}

	int set = cache.get_set(address);
	for(unsigned int way = 0; way < cache.NUM_WAY; way++)
		if(cache.block[set][way].state != I_STATE && cache.block[set][way].tag == address)
		{
			
				cache.block[set][way].state = I_STATE;
				return true;
		}

	return true;
}

bool CACHE::back_invalidate_l1(uint64_t address)
{
	assert(cache_type == IS_L2C);
    if(address == 0)
	    return true;
    return make_inclusive(ooo_cpu[cpu].L1I,address) && make_inclusive(ooo_cpu[cpu].L1D,address);
}

void CACHE::l1_handle_fill()
{

	assert(cache_type == IS_L1D || cache_type == IS_L1I);

    // handle fill
    uint32_t fill_cpu = (MSHR.next_fill_index == MSHR_SIZE) ? NUM_CPUS : MSHR.entry[MSHR.next_fill_index].cpu;
    if (fill_cpu == NUM_CPUS)
        return;

    if (MSHR.next_fill_cycle <= current_core_cycle[fill_cpu]) {

#ifdef SANITY_CHECK
        if (MSHR.next_fill_index >= MSHR.SIZE)
            assert(0);
#endif

        uint32_t mshr_index = MSHR.next_fill_index;

        // find victim
        uint32_t set = get_set(MSHR.entry[mshr_index].address);
	
	int way;
	//Coherence
	way = check_hit(&MSHR.entry[mshr_index]);

	if(way != -1)
	{
		//L1R4C3
		assert(block[set][way].state != M_STATE);
	
		//L1R3C3
		block[set][way].state = M_STATE; // If block is present in L1 then only upgrade miss can happen.
			
		  // COLLECT STATS
	    sim_miss[fill_cpu][MSHR.entry[mshr_index].type]++;
	    sim_access[fill_cpu][MSHR.entry[mshr_index].type]++;

	    // check fill level
	    if (MSHR.entry[mshr_index].fill_level < fill_level) 
		{
			if (MSHR.entry[mshr_index].instruction)
		    	upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
			else // data
		    	upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
	    }

	    // update processed packets
        if (cache_type == IS_L1I) 
		{
            if (PROCESSED.occupancy < PROCESSED.SIZE)
                PROCESSED.add_queue(&MSHR.entry[mshr_index]);
        }
        //else if (cache_type == IS_L1D) {
        else if ((cache_type == IS_L1D) && (MSHR.entry[mshr_index].type != PREFETCH)) 
		{
            if (PROCESSED.occupancy < PROCESSED.SIZE)
                PROCESSED.add_queue(&MSHR.entry[mshr_index]);
        }

	    if(warmup_complete[fill_cpu])
	    {
			uint64_t current_miss_latency = (current_core_cycle[fill_cpu] - MSHR.entry[mshr_index].cycle_enqueued);
			total_miss_latency += current_miss_latency;
	    }

	    MSHR.remove_queue(&MSHR.entry[mshr_index]);
	    MSHR.num_returned--;

	    update_fill_cycle();

	    return; // return here, as miss request is serviced.


	}

        way = find_victim(fill_cpu, MSHR.entry[mshr_index].instr_id, set, block[set], MSHR.entry[mshr_index].ip, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type);

        uint8_t  do_fill = 1;

        // is this dirty? //L1R4C4
        if (block[set][way].state == M_STATE) {

            // check if the lower level WQ has enough room to keep this writeback request
            if (lower_level) {
                if (lower_level->get_occupancy(2, block[set][way].address) == lower_level->get_size(2, block[set][way].address)) {

                    // lower level WQ is full, cannot replace this victim
                    do_fill = 0;
                    lower_level->increment_WQ_FULL(block[set][way].address);
                    STALL[MSHR.entry[mshr_index].type]++;

                    DP ( if (warmup_complete[fill_cpu]) {
                    cout << "[" << NAME << "] " << __func__ << "do_fill: " << +do_fill;
                    cout << " lower level wq is full!" << " fill_addr: " << hex << MSHR.entry[mshr_index].address;
                    cout << " victim_addr: " << block[set][way].tag << dec << endl; });
                }
                else {
                    PACKET writeback_packet;

                    writeback_packet.fill_level = fill_level << 1;
                    writeback_packet.cpu = fill_cpu;
                    writeback_packet.address = block[set][way].address;
                    writeback_packet.full_addr = block[set][way].full_addr;
                    writeback_packet.data = block[set][way].data;
                    writeback_packet.instr_id = MSHR.entry[mshr_index].instr_id;
                    writeback_packet.ip = 0; // writeback does not have ip
                    writeback_packet.type = WRITEBACK;
                    writeback_packet.event_cycle = current_core_cycle[fill_cpu];

                    lower_level->add_wq(&writeback_packet);
                }
            }
#ifdef SANITY_CHECK
            else {
                // sanity check
                if (cache_type != IS_STLB)
                    assert(0);
            }
#endif
        }

        if (do_fill){
            // update prefetcher
            if (cache_type == IS_L1D)
	      l1d_prefetcher_cache_fill(MSHR.entry[mshr_index].full_addr, set, way, (MSHR.entry[mshr_index].type == PREFETCH) ? 1 : 0, block[set][way].address<<LOG2_BLOCK_SIZE,
					MSHR.entry[mshr_index].pf_metadata);
              
            // update replacement policy
           update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, block[set][way].full_addr, MSHR.entry[mshr_index].type, 0);

            // COLLECT STATS
            sim_miss[fill_cpu][MSHR.entry[mshr_index].type]++;
            sim_access[fill_cpu][MSHR.entry[mshr_index].type]++;

            fill_cache(set, way, &MSHR.entry[mshr_index]);
	
    	    //Coherence: L1R1C3 	    
		    if(cache_type == IS_L1D)
		    {
		    	if(MSHR.entry[mshr_index].type == LOAD)
			    	block[set][way].state = S_STATE;
			    else if(MSHR.entry[mshr_index].type == RFO)
			    	block[set][way].state = M_STATE;
		    }
	    
            // check fill level
            if (MSHR.entry[mshr_index].fill_level < fill_level) {

                if (MSHR.entry[mshr_index].instruction) 
                    upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                else // data
                    upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
            }

            // update processed packets
            if (cache_type == IS_L1I) {
                if (PROCESSED.occupancy < PROCESSED.SIZE)
                    PROCESSED.add_queue(&MSHR.entry[mshr_index]);
            }
            //else if (cache_type == IS_L1D) {
            else if ((cache_type == IS_L1D) && (MSHR.entry[mshr_index].type != PREFETCH)) {
                if (PROCESSED.occupancy < PROCESSED.SIZE)
                    PROCESSED.add_queue(&MSHR.entry[mshr_index]);
            }

	    if(warmup_complete[fill_cpu])
	      {
			uint64_t current_miss_latency = (current_core_cycle[fill_cpu] - MSHR.entry[mshr_index].cycle_enqueued);
			total_miss_latency += current_miss_latency;
	      }
	  
            MSHR.remove_queue(&MSHR.entry[mshr_index]);
            MSHR.num_returned--;

            update_fill_cycle();
        }
    }
}

void CACHE::l1_handle_writeback()
{

    // handle write
    uint32_t writeback_cpu = WQ.entry[WQ.head].cpu;
    if (writeback_cpu == NUM_CPUS)
        return;

    assert(cache_type == IS_L1D); //Write request can't come to L1I

	// handle the oldest entry
	if ((WQ.entry[WQ.head].event_cycle <= current_core_cycle[writeback_cpu]) && (WQ.occupancy > 0)) {
        int index = WQ.head;

        // access cache
        uint32_t set = get_set(WQ.entry[index].address);
        int way = check_hit(&WQ.entry[index]);

        //Coherence: L1R3C2
        //Todo: Add a counter for coherence misses.
	if(cache_type == IS_L1D && way >= 0 && block[set][way].state == S_STATE) //Coherence Write miss
    	{
    		way = -1;
    	}
       

        if (way >= 0) { // writeback hit (or RFO hit for L1D)

                update_replacement_state(writeback_cpu, set, way, block[set][way].full_addr, WQ.entry[index].ip, 0, WQ.entry[index].type, 1);

            // COLLECT STATS
            sim_hit[writeback_cpu][WQ.entry[index].type]++;
            sim_access[writeback_cpu][WQ.entry[index].type]++;

            // mark dirty
            //block[set][way].dirty = 1;
	      block[set][way].state = M_STATE; 

            // check fill level
            if (WQ.entry[index].fill_level < fill_level) {

                if (WQ.entry[index].instruction) 
                    upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
                else // data
                    upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
            }

            HIT[WQ.entry[index].type]++;
            ACCESS[WQ.entry[index].type]++;

            // remove this entry from WQ
            WQ.remove_queue(&WQ.entry[index]);
        }
        else { // writeback miss (or RFO miss for L1D)
            
            DP ( if (warmup_complete[writeback_cpu]) {
            cout << "[" << NAME << "] " << __func__ << " type: " << +WQ.entry[index].type << " miss";
            cout << " instr_id: " << WQ.entry[index].instr_id << " address: " << hex << WQ.entry[index].address;
            cout << " full_addr: " << WQ.entry[index].full_addr << dec;
            cout << " cycle: " << WQ.entry[index].event_cycle << endl; });

            // check mshr
            uint8_t miss_handled = 1;
            int mshr_index = check_mshr(&WQ.entry[index]);

            if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE)) { // this is a new miss
	  
			      // add it to mshr (RFO miss)
			      add_mshr(&WQ.entry[index]);
			      
			      // add it to the next level's read queue
			      //if (lower_level) // L1D always has a lower level cache
			      lower_level->add_rq(&WQ.entry[index]);
            }
            else {

            	//Coherence: L1 Write request MSHR hit/MSHR full stall.

               //if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE)) { // not enough MSHR resource
                    
                    // cannot handle miss request until one of MSHRs is available
                    miss_handled = 0;
                    STALL[WQ.entry[index].type]++;
                /*}
                else if (mshr_index != -1) { // already in-flight miss

                    // update fill_level
                    if (WQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
                        MSHR.entry[mshr_index].fill_level = WQ.entry[index].fill_level;

                    // update request
                    if (MSHR.entry[mshr_index].type == PREFETCH) {
                        uint8_t  prior_returned = MSHR.entry[mshr_index].returned;
                        uint64_t prior_event_cycle = MSHR.entry[mshr_index].event_cycle;
		    			MSHR.entry[mshr_index] = WQ.entry[index];

                        // in case request is already returned, we should keep event_cycle and retunred variables
                        MSHR.entry[mshr_index].returned = prior_returned;
                        MSHR.entry[mshr_index].event_cycle = prior_event_cycle;
                    }

                    MSHR_MERGED[WQ.entry[index].type]++;

                    DP ( if (warmup_complete[writeback_cpu]) {
                    cout << "[" << NAME << "] " << __func__ << " mshr merged";
                    cout << " instr_id: " << WQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                    cout << " address: " << hex << WQ.entry[index].address;
                    cout << " full_addr: " << WQ.entry[index].full_addr << dec;
                    cout << " cycle: " << WQ.entry[index].event_cycle << endl; });
                }
                else { // WE SHOULD NOT REACH HERE
                    cerr << "[" << NAME << "] MSHR errors" << endl;
                    assert(0);
                }*/
            }

            if (miss_handled) {

                MISS[WQ.entry[index].type]++;
                ACCESS[WQ.entry[index].type]++;

                // remove this entry from WQ
                WQ.remove_queue(&WQ.entry[index]);
            }
        }
    }
}

void CACHE::l1_handle_read()
{
	
	assert(cache_type == IS_L1D || cache_type == IS_L1I);
    // handle read

    for (uint32_t i=0; i<MAX_READ; i++) {

      uint32_t read_cpu = RQ.entry[RQ.head].cpu;
      if (read_cpu == NUM_CPUS)
        return;

        // handle the oldest entry
        if ((RQ.entry[RQ.head].event_cycle <= current_core_cycle[read_cpu]) && (RQ.occupancy > 0)) {
            int index = RQ.head;

            // access cache
            uint32_t set = get_set(RQ.entry[index].address);
            int way = check_hit(&RQ.entry[index]);
            
            if (way >= 0) { // read hit

              if (cache_type == IS_L1I) {
                    if (PROCESSED.occupancy < PROCESSED.SIZE)
                        PROCESSED.add_queue(&RQ.entry[index]);
                }
                //else if (cache_type == IS_L1D) {
                else if ((cache_type == IS_L1D) && (RQ.entry[index].type != PREFETCH)) {
                    if (PROCESSED.occupancy < PROCESSED.SIZE)
                        PROCESSED.add_queue(&RQ.entry[index]);
                }

                // update prefetcher on load instruction
		if (RQ.entry[index].type == LOAD) {
                    if (cache_type == IS_L1D) 
		      l1d_prefetcher_operate(RQ.entry[index].full_addr, RQ.entry[index].ip, 1, RQ.entry[index].type);
         
                }

                // update replacement policy
                update_replacement_state(read_cpu, set, way, block[set][way].full_addr, RQ.entry[index].ip, 0, RQ.entry[index].type, 1);

                // COLLECT STATS
                sim_hit[read_cpu][RQ.entry[index].type]++;
                sim_access[read_cpu][RQ.entry[index].type]++;

                // check fill level
                if (RQ.entry[index].fill_level < fill_level) {

                    if (RQ.entry[index].instruction) 
                        upper_level_icache[read_cpu]->return_data(&RQ.entry[index]);
                    else // data
                        upper_level_dcache[read_cpu]->return_data(&RQ.entry[index]);
                }

                // update prefetch stats and reset prefetch bit
                if (block[set][way].prefetch) {
                    pf_useful++;
                    block[set][way].prefetch = 0;
                }
                block[set][way].used = 1;

                HIT[RQ.entry[index].type]++;
                ACCESS[RQ.entry[index].type]++;
                
                // remove this entry from RQ
                RQ.remove_queue(&RQ.entry[index]);
		reads_available_this_cycle--;
            }
            else { // read miss

                DP ( if (warmup_complete[read_cpu]) {
                cout << "[" << NAME << "] " << __func__ << " read miss";
                cout << " instr_id: " << RQ.entry[index].instr_id << " address: " << hex << RQ.entry[index].address;
                cout << " full_addr: " << RQ.entry[index].full_addr << dec;
                cout << " cycle: " << RQ.entry[index].event_cycle << endl; });

                // check mshr
                uint8_t miss_handled = 1;
                int mshr_index = check_mshr(&RQ.entry[index]);

                if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE)) { // this is a new miss
				
					  // add it to mshr (read miss)
					  add_mshr(&RQ.entry[index]);
					  
					  // add it to the next level's read queue
					  if (lower_level)
								lower_level->add_rq(&RQ.entry[index]);
                }
                else {
                    if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE)) { // not enough MSHR resource
                        
                        // cannot handle miss request until one of MSHRs is available
                        miss_handled = 0;
                        STALL[RQ.entry[index].type]++;
                    }
                    else if (mshr_index != -1) { // already in-flight miss

                        // mark merged consumer
                        if (RQ.entry[index].type == RFO) {

                            if (RQ.entry[index].tlb_access) {
                                uint32_t sq_index = RQ.entry[index].sq_index;
                                MSHR.entry[mshr_index].store_merged = 1;
                                MSHR.entry[mshr_index].sq_index_depend_on_me.insert (sq_index);
				MSHR.entry[mshr_index].sq_index_depend_on_me.join (RQ.entry[index].sq_index_depend_on_me, SQ_SIZE);
                            }

                            if (RQ.entry[index].load_merged) {
                                //uint32_t lq_index = RQ.entry[index].lq_index; 
                                MSHR.entry[mshr_index].load_merged = 1;
                                //MSHR.entry[mshr_index].lq_index_depend_on_me[lq_index] = 1;
				MSHR.entry[mshr_index].lq_index_depend_on_me.join (RQ.entry[index].lq_index_depend_on_me, LQ_SIZE);
                            }
                        }
                        else {
                            if (RQ.entry[index].instruction) {
                                uint32_t rob_index = RQ.entry[index].rob_index;
                                MSHR.entry[mshr_index].instr_merged = 1;
                                MSHR.entry[mshr_index].rob_index_depend_on_me.insert (rob_index);

                                DP (if (warmup_complete[MSHR.entry[mshr_index].cpu]) {
                                cout << "[INSTR_MERGED] " << __func__ << " cpu: " << MSHR.entry[mshr_index].cpu << " instr_id: " << MSHR.entry[mshr_index].instr_id;
                                cout << " merged rob_index: " << rob_index << " instr_id: " << RQ.entry[index].instr_id << endl; });

                                if (RQ.entry[index].instr_merged) {
				    MSHR.entry[mshr_index].rob_index_depend_on_me.join (RQ.entry[index].rob_index_depend_on_me, ROB_SIZE);
                                    DP (if (warmup_complete[MSHR.entry[mshr_index].cpu]) {
                                    cout << "[INSTR_MERGED] " << __func__ << " cpu: " << MSHR.entry[mshr_index].cpu << " instr_id: " << MSHR.entry[mshr_index].instr_id;
                                    cout << " merged rob_index: " << i << " instr_id: N/A" << endl; });
                                }
                            }
                            else 
                            {
                                uint32_t lq_index = RQ.entry[index].lq_index;
                                MSHR.entry[mshr_index].load_merged = 1;
                                MSHR.entry[mshr_index].lq_index_depend_on_me.insert (lq_index);

                                DP (if (warmup_complete[read_cpu]) {
                                cout << "[DATA_MERGED] " << __func__ << " cpu: " << read_cpu << " instr_id: " << RQ.entry[index].instr_id;
                                cout << " merged rob_index: " << RQ.entry[index].rob_index << " instr_id: " << RQ.entry[index].instr_id << " lq_index: " << RQ.entry[index].lq_index << endl; });
				MSHR.entry[mshr_index].lq_index_depend_on_me.join (RQ.entry[index].lq_index_depend_on_me, LQ_SIZE);
                                if (RQ.entry[index].store_merged) {
                                    MSHR.entry[mshr_index].store_merged = 1;
				    MSHR.entry[mshr_index].sq_index_depend_on_me.join (RQ.entry[index].sq_index_depend_on_me, SQ_SIZE);
                                }
                            }
                        }

                        // update fill_level
                        if (RQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
                            MSHR.entry[mshr_index].fill_level = RQ.entry[index].fill_level;

                        // update request
                        if (MSHR.entry[mshr_index].type == PREFETCH) {
                            uint8_t  prior_returned = MSHR.entry[mshr_index].returned;
                            uint64_t prior_event_cycle = MSHR.entry[mshr_index].event_cycle;
                            MSHR.entry[mshr_index] = RQ.entry[index];
                            
                            // in case request is already returned, we should keep event_cycle and retunred variables
                            MSHR.entry[mshr_index].returned = prior_returned;
                            MSHR.entry[mshr_index].event_cycle = prior_event_cycle;
                        }

                        MSHR_MERGED[RQ.entry[index].type]++;

                        DP ( if (warmup_complete[read_cpu]) {
                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
                        cout << " instr_id: " << RQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                        cout << " address: " << hex << RQ.entry[index].address;
                        cout << " full_addr: " << RQ.entry[index].full_addr << dec;
                        cout << " cycle: " << RQ.entry[index].event_cycle << endl; });
                    }
                    else { // WE SHOULD NOT REACH HERE
                        cerr << "[" << NAME << "] MSHR errors" << endl;
                        assert(0);
                    }
                }

                if (miss_handled) {
                    // update prefetcher on load instruction
		    if (RQ.entry[index].type == LOAD) {
                        if (cache_type == IS_L1D) 
                            l1d_prefetcher_operate(RQ.entry[index].full_addr, RQ.entry[index].ip, 0, RQ.entry[index].type);
                    }

                    MISS[RQ.entry[index].type]++;
                    ACCESS[RQ.entry[index].type]++;

                    // remove this entry from RQ
                    RQ.remove_queue(&RQ.entry[index]);
		    reads_available_this_cycle--;
                }
            }
        }
	else
	  {
	    return;
	  }

	if(reads_available_this_cycle == 0)
	  {
	    return;
	  }
    }
}








void CACHE::l2_handle_forwards()
{
	assert(cache_type == IS_L2C);

    if (FWQ.entry[FWQ.head].cpu == NUM_CPUS)
        return;

    // handle the oldest entry
    if ((FWQ.entry[FWQ.head].event_cycle <= current_core_cycle[FWQ.entry[FWQ.head].cpu]) && (FWQ.occupancy > 0)) 
    {
        int index = FWQ.head;

        // access cache
        uint32_t set = get_set(FWQ.entry[index].address);
        int way = check_hit(&FWQ.entry[index]);

        if(way >= 0)
        {
        	if(block[set][way].state == S_STATE)
        	{
        		assert(FWQ.entry[index].message_type == INV_MSG); //L2R5C(5,6,8)

        		//L2R5C7
        		if(back_invalidate_l1(FWQ.entry[index].address))
        		{
        			block[set][way].state = I_STATE;
        			FWQ.entry[index].message_type = INV_ACK_MSG;
        			ooo_cpu[FWQ.entry[index].requester_cpu].L2C.RESQ.add_queue(&FWQ.entry[index]);
        			FWQ.remove_queue(&FWQ.entry[index]);
        		}
        		else
        		{
                   	STALL[FWQ.entry[index].type]++;
        		}

        	}
        	else if(block[set][way].state == M_STATE)
        	{
        		assert(FWQ.entry[index].message_type == FWD_GETS_MSG || FWQ.entry[index].message_type == FWD_GETM_MSG); //L2R8C(7,8)

        		if(FWQ.entry[index].message_type == FWD_GETS_MSG) //L2R8C5
        		{
        			//Downgrade block in L1 to S state
        			uint64_t address = FWQ.entry[index].address;
        			CACHE &cache_L1D = ooo_cpu[cpu].L1D;
        			int set = cache_L1D.get_set(address);
					for(unsigned int way = 0; way < cache_L1D.NUM_WAY; way++)
						if(cache_L1D.block[set][way].state != I_STATE && cache_L1D.block[set][way].tag == address)
								cache_L1D.block[set][way].state = S_STATE;

					CACHE &cache_L1I = ooo_cpu[cpu].L1I;
        			set = cache_L1I.get_set(address);
					for(unsigned int way = 0; way < cache_L1I.NUM_WAY; way++)
						if(cache_L1I.block[set][way].state != I_STATE && cache_L1I.block[set][way].tag == address)
								cache_L1I.block[set][way].state = S_STATE;

				    //Send data to requester and directory
					block[set][way].state = S_STATE;
        			FWQ.entry[index].message_type = DATA_MSG;
        			ooo_cpu[FWQ.entry[index].requester_cpu].L2C.RESQ.add_queue(&FWQ.entry[index]);
        			uncore.LLC.RESQ.add_queue(&FWQ.entry[index]);
        			FWQ.remove_queue(&FWQ.entry[index]);
        		}
        		else if(FWQ.entry[index].message_type == FWD_GETM_MSG) //L2R8C6
        		{
        			//L2R8C6
	        		if(back_invalidate_l1(FWQ.entry[index].address))
	        		{
	        			block[set][way].state = I_STATE;
	        			FWQ.entry[index].message_type = DATA_MSG;
	        			ooo_cpu[FWQ.entry[index].requester_cpu].L2C.RESQ.add_queue(&FWQ.entry[index]);
	        			FWQ.remove_queue(&FWQ.entry[index]);
	        		}
	        		else
	        		{
	                   	STALL[FWQ.entry[index].type]++;
	        		}
        		}
        	}
        }
        else
        {
        	uint8_t request_handled = 1;
        	// check mshr
            int mshr_index = check_mshr(&FWQ.entry[index]);
            
            if(FWQ.entry[index].message_type == PUT_ACK_MSG)
            {
            	assert(mshr_index == -1);//L2R(1-4,6-7)C8
            	request_handled = 0;
            }
            else
            {
            	assert(mshr_index != -1); //L2R1C(5-7)
            	if(MSHR.entry[index].state == ISD_STATE)
	            {
	            	assert(FWQ.entry[index].message_type == INV_MSG); //L2R2C(5,6)
	            	request_handled = 0; //L2R2C7
	            }
	            else if(MSHR.entry[index].state == IMAD_STATE || MSHR.entry[index].state == IMA_STATE || MSHR.entry[index].state == SMA_STATE)
	            {
	            	assert(FWQ.entry[index].message_type != INV_MSG); //L2R(3,4,7)C7
	            	request_handled = 0; //L2R(3,4,7)C(5,6)
	            }
	            else if(MSHR.entry[index].state == SMAD_STATE)
	            {
	            	if(FWQ.entry[index].message_type == INV_MSG)
	            	{
	            		//L2R6C7
		        		if(back_invalidate_l1(FWQ.entry[index].address))
		        		{
		        			block[set][way].state = IMAD_STATE;
		        			FWQ.entry[index].message_type = INV_ACK_MSG;
		        			ooo_cpu[FWQ.entry[index].requester_cpu].L2C.RESQ.add_queue(&FWQ.entry[index]);
		        			FWQ.remove_queue(&FWQ.entry[index]);
		        		}
		        		else
		                   	request_handled = 0;
	            	}
	            	else
	            	{
	            		//L2R6C(5,6)
	            		request_handled = 0; 
	            	}
	            }
	            else
	            {
	            	assert(0); //No other transient state should be there
	            }
            }

            if(!request_handled)
            {
            	int fab_index = check_fab(&FWQ.entry[index]);

            	if(FWQ.entry[index].message_type == PUT_ACK_MSG)
            	{
            		//L2R(9-11)C8
            		assert(fab_index != -1);
            		FAB.remove_queue(&FAB.entry[fab_index]);
			FWQ.remove_queue(&FWQ.entry[index]);
            	}
            	else
            	{
            		if(FAB.entry[fab_index].state == MIA_STATE)
            		{
            			assert(FWQ.entry[index].message_type != INV_MSG); //L2R9C7

            			if(FWQ.entry[index].message_type == FWD_GETS_MSG) //L2R9C5
            			{
            				FAB.entry[fab_index].state = SIA_STATE;
		        			FWQ.entry[index].message_type = DATA_MSG;
		        			ooo_cpu[FWQ.entry[index].requester_cpu].L2C.RESQ.add_queue(&FWQ.entry[index]);
		        			uncore.LLC.RESQ.add_queue(&FWQ.entry[index]);
		        			FWQ.remove_queue(&FWQ.entry[index]);
            			}
            			else if(FWQ.entry[index].message_type == FWD_GETM_MSG) //L2R9C6
            			{
            				FAB.entry[fab_index].state = IIA_STATE;
		        			FWQ.entry[index].message_type = DATA_MSG;
		        			ooo_cpu[FWQ.entry[index].requester_cpu].L2C.RESQ.add_queue(&FWQ.entry[index]);
		        			FWQ.remove_queue(&FWQ.entry[index]);
            			}
            		}
            		else if(FAB.entry[fab_index].state == SIA_STATE)
            		{
            			assert(FWQ.entry[index].message_type == INV_MSG); //L2R10C(5,6)

            			//L2R10C7
            			FAB.entry[fab_index].state = IIA_STATE;
	        			FWQ.entry[index].message_type = INV_ACK_MSG;
	        			ooo_cpu[FWQ.entry[index].requester_cpu].L2C.RESQ.add_queue(&FWQ.entry[index]);
	        			FWQ.remove_queue(&FWQ.entry[index]);
            		}
            		else if(FAB.entry[fab_index].state == IIA_STATE)
            		{
            			assert(0); //L2R11C(5-7)
            		}
            		else
            		{
            			assert(0); //No other transient state should be there
            		}
            	}
            }	
        }
    }
}

int CACHE::l2_handle_fill(uint32_t mshr_index) //Return way if fill successfull as return -1
{

	uint32_t set = get_set(MSHR.entry[mshr_index].address), way;

	uint32_t fill_cpu = MSHR.entry[mshr_index].cpu;

    way = find_victim(fill_cpu, MSHR.entry[mshr_index].instr_id, set, block[set], MSHR.entry[mshr_index].ip, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type);

    uint8_t  do_fill = 1;

    //MSHR hit then stall //L2R(2,3,4,6,7)C4
    for (uint32_t index=0; index<MSHR_SIZE; index++) {
        if ( MSHR.entry[index].address != 0 && MSHR.entry[index].address == block[set][way].address) {
    	   //assert(0); //Remove this      
            return -1;
        }
    }

    //MSHR hit then stall //L2R(9-11)C4
    for (uint32_t index=0; index<L2C_FAB_SIZE; index++) {
        if ( FAB.entry[index].address != 0 && FAB.entry[index].address == block[set][way].address) {
           
            assert(0);
        }
    }

    // is this dirty?
    
    if(block[set][way].address != 0)
    {
	    if (lower_level) {
	        if (lower_level->get_occupancy(4, block[set][way].address) == lower_level->get_size(4, block[set][way].address)) {

	            // lower level WQ is full, cannot replace this victim
	            do_fill = 0;
	            lower_level->increment_WQ_FULL(block[set][way].address);
	            STALL[MSHR.entry[mshr_index].type]++;
		    //assert(0);//Remove this
	            return -1;
	        }
	        else {

	        	if(back_invalidate_l1(block[set][way].address))
	    		{
	    			PACKET put_packet;

		            put_packet.fill_level = fill_level << 1;
		            put_packet.cpu = fill_cpu;
		            put_packet.address = block[set][way].address;
		            put_packet.full_addr = block[set][way].full_addr;
		            put_packet.data = block[set][way].data;
		            put_packet.instr_id = MSHR.entry[mshr_index].instr_id;
		            put_packet.ip = 0; // writeback does not have ip
		            if(block[set][way].state == M_STATE)
		            	put_packet.message_type = PUTM_MSG;
		            else if(block[set][way].state == S_STATE)
		            	put_packet.message_type = PUTS_MSG;
		            put_packet.event_cycle = current_core_cycle[fill_cpu];

                    assert(put_packet.message_type!=5);
		            uncore.LLC.REQQ.add_queue(&put_packet);
		            if(block[set][way].state == M_STATE)
		            	put_packet.state = MIA_STATE;
		            else if(block[set][way].state == S_STATE)
		            	put_packet.state = SIA_STATE;
		            FAB.add_queue(&put_packet);
	    		}
	    		else
	    		{
	               		STALL[MSHR.entry[mshr_index].type]++;
				//assert(0);//remove this
	               		return -1;
	    		}

	            
	        }
	    }
	}

    if (do_fill){
        // update prefetcher
        if  (cache_type == IS_L2C)
      MSHR.entry[mshr_index].pf_metadata = l2c_prefetcher_cache_fill(MSHR.entry[mshr_index].address<<LOG2_BLOCK_SIZE, set, way, (MSHR.entry[mshr_index].type == PREFETCH) ? 1 : 0,
								     block[set][way].address<<LOG2_BLOCK_SIZE, MSHR.entry[mshr_index].pf_metadata);
       
        // update replacement policy
        update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, block[set][way].full_addr, MSHR.entry[mshr_index].type, 0);

        // COLLECT STATS
        sim_miss[fill_cpu][MSHR.entry[mshr_index].type]++;
        sim_access[fill_cpu][MSHR.entry[mshr_index].type]++;

        fill_cache(set, way, &MSHR.entry[mshr_index]);

        if(MSHR.entry[mshr_index].state == ISD_STATE)
        {
        	block[set][way].state = S_STATE;
        }
        else if(MSHR.entry[mshr_index].state == IMAD_STATE || MSHR.entry[mshr_index].state == IMA_STATE || MSHR.entry[mshr_index].state == SMAD_STATE || MSHR.entry[mshr_index].state == SMA_STATE)
        {
        	block[set][way].state = M_STATE;
        }

        // check fill level
        if (MSHR.entry[mshr_index].fill_level < fill_level) {

            if (MSHR.entry[mshr_index].instruction) 
                upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
            else // data
                upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
        }

    if(warmup_complete[fill_cpu])
      {
		uint64_t current_miss_latency = (current_core_cycle[fill_cpu] - MSHR.entry[mshr_index].cycle_enqueued);
		total_miss_latency += current_miss_latency;
      }
  
        MSHR.remove_queue(&MSHR.entry[mshr_index]);
        MSHR.num_returned--;

        update_fill_cycle();
    }

    if(do_fill == 1)
    	return way;
    else return -1;
}


void CACHE::l2_handle_response()
{
	assert(cache_type == IS_L2C);

    if (RESQ.entry[RESQ.head].cpu == NUM_CPUS)
        return;

    // handle the oldest entry
    if ((RESQ.entry[RESQ.head].event_cycle <= current_core_cycle[RESQ.entry[RESQ.head].cpu]) && (RESQ.occupancy > 0)) 
    {
        int index = RESQ.head;

        // access cache
        uint32_t set = get_set(RESQ.entry[index].address);
        int way = check_hit(&RESQ.entry[index]);

        if(way >= 0)
        {
        	assert(block[set][way].state != M_STATE); //L2R8C(9-13)
        	assert(RESQ.entry[index].message_type != INV_ACK_MSG); //L2R5C(9-13)
        }

        assert(check_fab(&RESQ.entry[index]) == -1); //L2R(9-11)C(9-13)

        int mshr_index = check_mshr(&RESQ.entry[index]);

        assert(mshr_index != -1); //L2R1C(9-13)

        int response_handled = 1;

        if(RESQ.entry[index].message_type == DATA_MSG)
        {
        	assert(MSHR.entry[mshr_index].state != IMA_STATE); //L2R4C(9-11)

        	if(RESQ.entry[index].acks == 0)
        	{
        		if(way >= 0) 
        		{
        			assert(block[set][way].state == S_STATE); //Block should be in S state
        			assert(MSHR.entry[mshr_index].state != SMA_STATE); // R7C(9-11)

        			block[set][way].state = M_STATE;// L2R6C(9,11)
        			if (MSHR.entry[mshr_index].fill_level < fill_level) {
			            if (MSHR.entry[mshr_index].instruction) 
			                upper_level_icache[cpu]->return_data(&MSHR.entry[mshr_index]);
			            else // data
			                upper_level_dcache[cpu]->return_data(&MSHR.entry[mshr_index]);
			        }

			        MSHR.remove_queue(&MSHR.entry[mshr_index]);
			        MSHR.num_returned--;

			        update_fill_cycle();

        		}
        		else
        		{
        			int fill_way = l2_handle_fill(mshr_index); //L2R(2,3)C(9,11)
        			if(fill_way == -1)
				{
        				response_handled = 0;
					//assert(0);//@Vishal: Remove this
				}
        		}
        	}
        	else
        	{
        		assert(MSHR.entry[mshr_index].state != ISD_STATE);

        		//L2R(3,6)C10
        		if(MSHR.entry[mshr_index].state == IMAD_STATE)
        			MSHR.entry[mshr_index].state = IMA_STATE;
        		else if(MSHR.entry[mshr_index].state == SMAD_STATE)
        			MSHR.entry[mshr_index].state = SMA_STATE;
        		else
        			assert(0);
        	}
        }
        else if(RESQ.entry[index].message_type == INV_ACK_MSG)
        {
        	MSHR.entry[mshr_index].acks--; //L2R(3,4,6,7)C12

        	if(MSHR.entry[mshr_index].acks == 0)
        	{
        		//@Vishal: Check L2R(3,6)C13
        		int fill_way = l2_handle_fill(mshr_index); //L2R(4,7)C13
    			if(fill_way == -1)
    				response_handled = 0;
        	}
        }

        if(response_handled)
        {
        	RESQ.remove_queue(&RESQ.entry[index]);
        }
    }
}

void CACHE::l2_handle_writeback()
{

	assert(cache_type == IS_L2C);

    // handle write
    uint32_t writeback_cpu = WQ.entry[WQ.head].cpu;
    if (writeback_cpu == NUM_CPUS)
        return;

    // handle the oldest entry
    if ((WQ.entry[WQ.head].event_cycle <= current_core_cycle[writeback_cpu]) && (WQ.occupancy > 0)) {
        int index = WQ.head;

        // access cache
        uint32_t set = get_set(WQ.entry[index].address);
        int way = check_hit(&WQ.entry[index]);

        assert(way >= 0); //L1 Dirty blocks should be present in L2
        assert(block[set][way].state == M_STATE); //Block should be in modified state


        update_replacement_state(writeback_cpu, set, way, block[set][way].full_addr, WQ.entry[index].ip, 0, WQ.entry[index].type, 1);

        // COLLECT STATS
        sim_hit[writeback_cpu][WQ.entry[index].type]++;
        sim_access[writeback_cpu][WQ.entry[index].type]++;

        // mark dirty
        //block[set][way].dirty = 1;
      	block[set][way].state = M_STATE; 

        // check fill level
        if (WQ.entry[index].fill_level < fill_level) {

            if (WQ.entry[index].instruction) 
                upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
            else // data
                upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
        }

        HIT[WQ.entry[index].type]++;
        ACCESS[WQ.entry[index].type]++;

        // remove this entry from WQ
        WQ.remove_queue(&WQ.entry[index]);
    }
}

void CACHE::l2_handle_read()
{
	
	assert(cache_type == IS_L2C);
	
    // handle read

    for (uint32_t i=0; i<MAX_READ; i++) {

      uint32_t read_cpu = RQ.entry[RQ.head].cpu;
      if (read_cpu == NUM_CPUS)
        return;

        // handle the oldest entry
        if ((RQ.entry[RQ.head].event_cycle <= current_core_cycle[read_cpu]) && (RQ.occupancy > 0)) {
            int index = RQ.head;

            // access cache
            uint32_t set = get_set(RQ.entry[index].address);
            int way = check_hit(&RQ.entry[index]);

            //Coherence: L2R5C2
	        //Todo: Add a counter for coherence misses.
		if(way >= 0 && block[set][way].state == S_STATE && RQ.entry[index].type == RFO) //Coherence Write miss
		    {
		    	way = -1;
		    }
            
            if (way >= 0) { // read hit

            	if(RQ.entry[index].type == LOAD)
            		assert(block[set][way].state == S_STATE || block[set][way].state == M_STATE); // L2R5C1, L2R8C1
            	else if(RQ.entry[index].type == RFO)
            		assert(block[set][way].state == M_STATE); // L2R8C2
            	
                // update prefetcher on load instruction
				if (RQ.entry[index].type == LOAD) {
                     if (cache_type == IS_L2C)
		      			l2c_prefetcher_operate(block[set][way].address<<LOG2_BLOCK_SIZE, RQ.entry[index].ip, 1, RQ.entry[index].type, 0);
                }

                // update replacement policy
               	update_replacement_state(read_cpu, set, way, block[set][way].full_addr, RQ.entry[index].ip, 0, RQ.entry[index].type, 1);

                // COLLECT STATS
                sim_hit[read_cpu][RQ.entry[index].type]++;
                sim_access[read_cpu][RQ.entry[index].type]++;

                // check fill level
                if (RQ.entry[index].fill_level < fill_level) {

                    if (RQ.entry[index].instruction) 
                        upper_level_icache[read_cpu]->return_data(&RQ.entry[index]);
                    else // data
                        upper_level_dcache[read_cpu]->return_data(&RQ.entry[index]);
                }

                // update prefetch stats and reset prefetch bit
                if (block[set][way].prefetch) {
                    pf_useful++;
                    block[set][way].prefetch = 0;
                }
                block[set][way].used = 1;

                HIT[RQ.entry[index].type]++;
                ACCESS[RQ.entry[index].type]++;
                
                // remove this entry from RQ
                RQ.remove_queue(&RQ.entry[index]);
				reads_available_this_cycle--;
            }
            else { // read miss

                DP ( if (warmup_complete[read_cpu]) {
                cout << "[" << NAME << "] " << __func__ << " read miss";
                cout << " instr_id: " << RQ.entry[index].instr_id << " address: " << hex << RQ.entry[index].address;
                cout << " full_addr: " << RQ.entry[index].full_addr << dec;
                cout << " cycle: " << RQ.entry[index].event_cycle << endl; });

                // check mshr
                uint8_t miss_handled = 1;
                int mshr_index = check_mshr(&RQ.entry[index]);

                if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE)) { // this is a new miss

						  // add it to mshr (read miss)

                		  if(RQ.entry[index].type == LOAD) //L2R1C1
                		  {		
                		  		RQ.entry[index].message_type = GETS_MSG;
                		  		RQ.entry[index].state = ISD_STATE;	
                		  }
                		  else if(RQ.entry[index].type == RFO)
                		  {
                		  		RQ.entry[index].message_type = GETM_MSG;

                		  		if(check_hit(&RQ.entry[index]) != -1) //L2R5C2
                		  			RQ.entry[index].state = SMAD_STATE;
                		  		else	
                		  			RQ.entry[index].state = IMAD_STATE; //L2R1C2
                		  }
						  add_mshr(&RQ.entry[index]);
						  assert(RQ.entry[index].message_type!=5);
						  uncore.LLC.REQQ.add_queue(&RQ.entry[index]);
                }
                else {
                    if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE)) { // not enough MSHR resource
                        
                        // cannot handle miss request until one of MSHRs is available
                        miss_handled = 0;
                        STALL[RQ.entry[index].type]++;
                    }
                    else if (mshr_index != -1) { // already in-flight miss

                    	assert(MSHR.entry[mshr_index].state != I_STATE || MSHR.entry[mshr_index].state != S_STATE || MSHR.entry[mshr_index].state == M_STATE);
                        // mark merged consumer
                        if (RQ.entry[index].type == RFO) {
                           
                            //L2R(2-4,6-7,9-11)C2
                            miss_handled = 0; 
                            STALL[RQ.entry[index].type]++;

                        }
                        else {

                            if(MSHR.entry[mshr_index].state == SMAD_STATE || MSHR.entry[mshr_index].state == SMA_STATE)
                            {
                            	assert(0); //Should hit in cache L2R(6,7)C2
                            }
                            else
                            {
                            	assert(MSHR.entry[mshr_index].state != ISD_STATE); // L2R2C1
                            	miss_handled = 0; //L2R(3,4,9-11)C1
                            	STALL[RQ.entry[index].type]++;
                            }
                        }


                        if(miss_handled)
                        {
	                        // update fill_level
	                        if (RQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
	                            MSHR.entry[mshr_index].fill_level = RQ.entry[index].fill_level;

	                        // update request
	                        if (MSHR.entry[mshr_index].type == PREFETCH) {
	                            uint8_t  prior_returned = MSHR.entry[mshr_index].returned;
	                            uint64_t prior_event_cycle = MSHR.entry[mshr_index].event_cycle;
	                            MSHR.entry[mshr_index] = RQ.entry[index];
	                            
	                            // in case request is already returned, we should keep event_cycle and retunred variables
	                            MSHR.entry[mshr_index].returned = prior_returned;
	                            MSHR.entry[mshr_index].event_cycle = prior_event_cycle;
	                        }

	                        MSHR_MERGED[RQ.entry[index].type]++;

	                        DP ( if (warmup_complete[read_cpu]) {
	                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
	                        cout << " instr_id: " << RQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
	                        cout << " address: " << hex << RQ.entry[index].address;
	                        cout << " full_addr: " << RQ.entry[index].full_addr << dec;
	                        cout << " cycle: " << RQ.entry[index].event_cycle << endl; });
	                    }
                    }
                    else { // WE SHOULD NOT REACH HERE
                        cerr << "[" << NAME << "] MSHR errors" << endl;
                        assert(0);
                    }
                }

                if (miss_handled) {
                    // update prefetcher on load instruction
				    if (RQ.entry[index].type == LOAD) {
		                        if (cache_type == IS_L2C)
					  				l2c_prefetcher_operate(RQ.entry[index].address<<LOG2_BLOCK_SIZE, RQ.entry[index].ip, 0, RQ.entry[index].type, 0);
		                    }

                    MISS[RQ.entry[index].type]++;
                    ACCESS[RQ.entry[index].type]++;

                    // remove this entry from RQ
                    RQ.remove_queue(&RQ.entry[index]);
		    		reads_available_this_cycle--;
                }
            }
        }
	else
	  {
	    return;
	  }

	if(reads_available_this_cycle == 0)
	  {
	    return;
	  }
    }
}

















void CACHE::llc_handle_request()
{
	assert(cache_type == IS_LLC);

    if (REQQ.entry[REQQ.head].cpu == NUM_CPUS)
        return;

    // handle the oldest entry
    if ((REQQ.entry[REQQ.head].event_cycle <= current_core_cycle[REQQ.entry[REQQ.head].cpu]) && (REQQ.occupancy > 0)) 
    {
        int index = REQQ.head;
	
	assert(REQQ.entry[index].address != 0);

        // access cache
        uint32_t set = get_set(REQQ.entry[index].address);
        int dir_way = dir_check_hit(&REQQ.entry[index]);


	/*if(set ==  324 && (REQQ.entry[index].message_type == PUTS_MSG || REQQ.entry[index].message_type == PUTM_MSG)) //Remove This
	{
	 	cout<<"set: " <<set<<" way: "<<dir_way<<endl;
	}*/	

	if(dir_way != -1)
	{
		assert(directory[set][dir_way].sharers_cnt <= NUM_CPUS);
	}

        if(dir_way == -1)
        {
        	if(REQQ.entry[index].message_type == PUTS_MSG || REQQ.entry[index].message_type == PUTM_MSG) //LLCR1C(4-7)
        	{
    			REQQ.entry[index].message_type = PUT_ACK_MSG;
    			ooo_cpu[REQQ.entry[index].cpu].L2C.FWQ.add_queue(&REQQ.entry[index]);
    			REQQ.remove_queue(&REQQ.entry[index]);
        	}
        	else if(REQQ.entry[index].message_type == GETS_MSG || REQQ.entry[index].message_type == GETM_MSG) //LLCR1C(1,2)
        	{
        		for(dir_way = 0; dir_way < DIR_NUM_WAY;dir_way++)
        		{
        			if(directory[set][dir_way].state == I_STATE)
        				break;
        		}

        		assert(dir_way != DIR_NUM_WAY);// Directory should be able to track all blocks
			
			
    			directory[set][dir_way].sharers[REQQ.entry[index].cpu] = true;

    			if(REQQ.entry[index].message_type == GETS_MSG)
    				directory[set][dir_way].state = S_STATE;
    			else if(REQQ.entry[index].message_type == GETM_MSG)
    				directory[set][dir_way].state = M_STATE;
			else
				assert(0);
			
			directory[set][dir_way].address = REQQ.entry[index].address;
			directory[set][dir_way].full_addr = REQQ.entry[index].full_addr;
			directory[set][dir_way].tag = REQQ.entry[index].address;
    			directory[set][dir_way].instr_id = REQQ.entry[index].instr_id;
			directory[set][dir_way].sharers_cnt++;
			assert(directory[set][dir_way].sharers_cnt <= NUM_CPUS);

			REQQ.entry[index].message_type = DATA_MSG;
                        ooo_cpu[REQQ.entry[index].cpu].L2C.RESQ.add_queue(&REQQ.entry[index]);


    			REQQ.remove_queue(&REQQ.entry[index]);
        	}
        }
        else if(directory[set][dir_way].state == S_STATE)
        {
        	if(REQQ.entry[index].message_type == GETS_MSG) //LLCR2C1
        	{
        		//Add LLC Hit latency and just send the data 
	            	REQQ.entry[index].message_type = DATA_MSG;
    			ooo_cpu[REQQ.entry[index].cpu].L2C.RESQ.add_queue(&REQQ.entry[index]);

    			directory[set][dir_way].sharers[REQQ.entry[index].cpu] = true;
    			directory[set][dir_way].sharers_cnt++;
    			assert(directory[set][dir_way].sharers_cnt <= NUM_CPUS);

    			
    			REQQ.remove_queue(&REQQ.entry[index]);
        	}
        	else if(REQQ.entry[index].message_type == GETM_MSG) //LLCR2C2
        	{
        		//Add LLC Hit latency and just send the data 
	            REQQ.entry[index].message_type = DATA_MSG;
    			ooo_cpu[REQQ.entry[index].cpu].L2C.RESQ.add_queue(&REQQ.entry[index]);

    			for(int i = 0; i < NUM_CPUS; i++)
    				if( i!= REQQ.entry[index].cpu && directory[set][dir_way].sharers[i])
    				{
    					REQQ.entry[index].message_type = INV_MSG;
    					ooo_cpu[i].L2C.FWQ.add_queue(&REQQ.entry[index]);
    					directory[set][dir_way].sharers[i] = false;
    				}

    			directory[set][dir_way].sharers[REQQ.entry[index].cpu] = true;
    			directory[set][dir_way].state = M_STATE;
    			directory[set][dir_way].sharers_cnt = 1;
    			
    			REQQ.remove_queue(&REQQ.entry[index]);
        	}
        	else if(REQQ.entry[index].message_type == PUTS_MSG || REQQ.entry[index].message_type == PUTM_MSG) //LLCR2C(4-7)
        	{

        		if(REQQ.entry[index].message_type == PUTM_MSG) //LLCR2C6
        		{
        			assert(directory[set][dir_way].sharers_cnt == 1);
        			for(uint16_t i = 0; i < NUM_CPUS; i++)
	    				if(directory[set][dir_way].sharers[i])
	    					assert(i == REQQ.entry[index].cpu);
        		}

        		//Add LLC Hit latency and just send the data 
	            	REQQ.entry[index].message_type = PUT_ACK_MSG;
    			ooo_cpu[REQQ.entry[index].cpu].L2C.FWQ.add_queue(&REQQ.entry[index]);

    			directory[set][dir_way].sharers[REQQ.entry[index].cpu] = false;
    			directory[set][dir_way].sharers_cnt--;
				assert(directory[set][dir_way].sharers_cnt <= NUM_CPUS);

    			if(directory[set][dir_way].sharers_cnt == 0)
    				directory[set][dir_way].state = I_STATE;
    			
    			REQQ.remove_queue(&REQQ.entry[index]);
        	}
        }
        else if(directory[set][dir_way].state == M_STATE)
        {
        	if(REQQ.entry[index].message_type == GETS_MSG) //LLCR3C1
        	{
        		assert(directory[set][dir_way].sharers_cnt == 1);
        		for(int i = 0; i < NUM_CPUS; i++)
    				if(directory[set][dir_way].sharers[i])
    				{
    					REQQ.entry[index].message_type = FWD_GETS_MSG;
    					ooo_cpu[i].L2C.FWQ.add_queue(&REQQ.entry[index]);
    					break;
    				}

    			directory[set][dir_way].sharers[REQQ.entry[index].cpu] = true;
    			directory[set][dir_way].sharers_cnt++;
				assert(directory[set][dir_way].sharers_cnt <= NUM_CPUS);

    			directory[set][dir_way].state = SD_STATE;
    			
    			REQQ.remove_queue(&REQQ.entry[index]);
        	}
        	else if(REQQ.entry[index].message_type == GETM_MSG) //LLCR2C2
        	{
        		assert(directory[set][dir_way].sharers_cnt == 1);
	          
    			for(int i = 0; i < NUM_CPUS; i++)
    				if(directory[set][dir_way].sharers[i])
    				{
    					REQQ.entry[index].message_type = FWD_GETM_MSG;
	            			REQQ.entry[index].requester_cpu = REQQ.entry[index].cpu;
    					ooo_cpu[i].L2C.RESQ.add_queue(&REQQ.entry[index]);
    					directory[set][dir_way].sharers[i] = false;
    				}

    			directory[set][dir_way].sharers[REQQ.entry[index].cpu] = true;
    			directory[set][dir_way].state = M_STATE;
    			directory[set][dir_way].sharers_cnt = 1;
    			
    			REQQ.remove_queue(&REQQ.entry[index]);
        	}
        	else if(REQQ.entry[index].message_type == PUTS_MSG || REQQ.entry[index].message_type == PUTM_MSG) //LLCR2C(4-7)
        	{

	            	REQQ.entry[index].message_type = PUT_ACK_MSG;
    			ooo_cpu[REQQ.entry[index].cpu].L2C.FWQ.add_queue(&REQQ.entry[index]);

    			directory[set][dir_way].sharers[REQQ.entry[index].cpu] = false;
    			directory[set][dir_way].sharers_cnt--;

    			if(directory[set][dir_way].sharers_cnt == 0)
    				directory[set][dir_way].state = I_STATE;
    			
    			REQQ.remove_queue(&REQQ.entry[index]);
        	}
        }
        else if(directory[set][dir_way].state == SD_STATE)
        {
        	if(REQQ.entry[index].message_type == GETS_MSG || REQQ.entry[index].message_type == GETM_MSG) //LLCR3C1
        	{
        		//Stall
        	}
        	else if(REQQ.entry[index].message_type == PUTS_MSG || REQQ.entry[index].message_type == PUTM_MSG) //LLCR2C(4-7)
        	{

	            REQQ.entry[index].message_type = PUT_ACK_MSG;
    			ooo_cpu[REQQ.entry[index].cpu].L2C.FWQ.add_queue(&REQQ.entry[index]);

    			directory[set][dir_way].sharers[REQQ.entry[index].cpu] = false;
    			directory[set][dir_way].sharers_cnt--;

    			assert(directory[set][dir_way].sharers_cnt != 0);
    			
    			REQQ.remove_queue(&REQQ.entry[index]);
        	}
        }
    }
}

void CACHE::llc_handle_response()
{
    assert(cache_type == IS_LLC);

    if (RESQ.entry[RESQ.head].cpu == NUM_CPUS)
        return;

    // handle the oldest entry
    if ((RESQ.entry[RESQ.head].event_cycle <= current_core_cycle[RESQ.entry[RESQ.head].cpu]) && (RESQ.occupancy > 0)) 
    {
    	int index = RESQ.head;

    	assert(RESQ.entry[RESQ.head].message_type == DATA_MSG);

        // access cache
        uint32_t set = get_set(RESQ.entry[index].address);
        int dir_way = dir_check_hit(&RESQ.entry[index]);

        assert(dir_way != -1); //LLCR1C8

        assert(directory[set][dir_way].state == SD_STATE); //LLCR(2,3)C8


		directory[set][dir_way].sharers[RESQ.entry[index].cpu] = false;
		directory[set][dir_way].sharers_cnt--;
		directory[set][dir_way].state = S_STATE;

		assert(directory[set][dir_way].sharers_cnt != 0);
		
		RESQ.remove_queue(&RESQ.entry[index]);

    }

}















void CACHE::handle_fill()
{
    // handle fill
    uint32_t fill_cpu = (MSHR.next_fill_index == MSHR_SIZE) ? NUM_CPUS : MSHR.entry[MSHR.next_fill_index].cpu;
    if (fill_cpu == NUM_CPUS)
        return;

    if (MSHR.next_fill_cycle <= current_core_cycle[fill_cpu]) {

#ifdef SANITY_CHECK
        if (MSHR.next_fill_index >= MSHR.SIZE)
            assert(0);
#endif

        uint32_t mshr_index = MSHR.next_fill_index;

        // find victim
        uint32_t set = get_set(MSHR.entry[mshr_index].address), way;

	
        way = find_victim(fill_cpu, MSHR.entry[mshr_index].instr_id, set, block[set], MSHR.entry[mshr_index].ip, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type);

        uint8_t  do_fill = 1;

        // is this dirty?
        if (block[set][way].state == M_STATE) {

            // check if the lower level WQ has enough room to keep this writeback request
            if (lower_level) {
                if (lower_level->get_occupancy(2, block[set][way].address) == lower_level->get_size(2, block[set][way].address)) {

                    // lower level WQ is full, cannot replace this victim
                    do_fill = 0;
                    lower_level->increment_WQ_FULL(block[set][way].address);
                    STALL[MSHR.entry[mshr_index].type]++;

                    DP ( if (warmup_complete[fill_cpu]) {
                    cout << "[" << NAME << "] " << __func__ << "do_fill: " << +do_fill;
                    cout << " lower level wq is full!" << " fill_addr: " << hex << MSHR.entry[mshr_index].address;
                    cout << " victim_addr: " << block[set][way].tag << dec << endl; });
                }
                else {
                    PACKET writeback_packet;

                    writeback_packet.fill_level = fill_level << 1;
                    writeback_packet.cpu = fill_cpu;
                    writeback_packet.address = block[set][way].address;
                    writeback_packet.full_addr = block[set][way].full_addr;
                    writeback_packet.data = block[set][way].data;
                    writeback_packet.instr_id = MSHR.entry[mshr_index].instr_id;
                    writeback_packet.ip = 0; // writeback does not have ip
                    writeback_packet.type = WRITEBACK;
                    writeback_packet.event_cycle = current_core_cycle[fill_cpu];

                    lower_level->add_wq(&writeback_packet);
                }
            }
#ifdef SANITY_CHECK
            else {
                // sanity check
                if (cache_type != IS_STLB)
                    assert(0);
            }
#endif
        }

        if (do_fill){
            // update prefetcher
         
              
            // update replacement policy
			update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, block[set][way].full_addr, MSHR.entry[mshr_index].type, 0);

            // COLLECT STATS
            sim_miss[fill_cpu][MSHR.entry[mshr_index].type]++;
            sim_access[fill_cpu][MSHR.entry[mshr_index].type]++;

            fill_cache(set, way, &MSHR.entry[mshr_index]);
	
            // check fill level
            if (MSHR.entry[mshr_index].fill_level < fill_level) {

                if (MSHR.entry[mshr_index].instruction) 
                    upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                else // data
                    upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
            }

            // update processed packets
            if (cache_type == IS_ITLB) { 
                MSHR.entry[mshr_index].instruction_pa = block[set][way].data;
                if (PROCESSED.occupancy < PROCESSED.SIZE)
                    PROCESSED.add_queue(&MSHR.entry[mshr_index]);
            }
            else if (cache_type == IS_DTLB) {
                MSHR.entry[mshr_index].data_pa = block[set][way].data;
                if (PROCESSED.occupancy < PROCESSED.SIZE)
                    PROCESSED.add_queue(&MSHR.entry[mshr_index]);
            }

		    if(warmup_complete[fill_cpu])
		      {
			uint64_t current_miss_latency = (current_core_cycle[fill_cpu] - MSHR.entry[mshr_index].cycle_enqueued);
			total_miss_latency += current_miss_latency;
		      }
		  
	            MSHR.remove_queue(&MSHR.entry[mshr_index]);
	            MSHR.num_returned--;

	            update_fill_cycle();
	        }
    }
}

void CACHE::handle_writeback()
{

    // handle write
    uint32_t writeback_cpu = WQ.entry[WQ.head].cpu;
    if (writeback_cpu == NUM_CPUS)
        return;

    assert(cache_type != IS_L1I); //Write request can't come to L1I

    // handle the oldest entry
    if ((WQ.entry[WQ.head].event_cycle <= current_core_cycle[writeback_cpu]) && (WQ.occupancy > 0)) {
        int index = WQ.head;

        // access cache
        uint32_t set = get_set(WQ.entry[index].address);
        int way = check_hit(&WQ.entry[index]);

        if (way >= 0) { // writeback hit (or RFO hit for L1D)

            update_replacement_state(writeback_cpu, set, way, block[set][way].full_addr, WQ.entry[index].ip, 0, WQ.entry[index].type, 1);

            // COLLECT STATS
            sim_hit[writeback_cpu][WQ.entry[index].type]++;
            sim_access[writeback_cpu][WQ.entry[index].type]++;

            // mark dirty
            //block[set][way].dirty = 1;
	      block[set][way].state = M_STATE; 


            if (cache_type == IS_ITLB)
                WQ.entry[index].instruction_pa = block[set][way].data;
            else if (cache_type == IS_DTLB)
                WQ.entry[index].data_pa = block[set][way].data;
            else if (cache_type == IS_STLB)
                WQ.entry[index].data = block[set][way].data;

            // check fill level
            if (WQ.entry[index].fill_level < fill_level) {

                if (WQ.entry[index].instruction) 
                    upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
                else // data
                    upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
            }

            HIT[WQ.entry[index].type]++;
            ACCESS[WQ.entry[index].type]++;

            // remove this entry from WQ
            WQ.remove_queue(&WQ.entry[index]);
        }
        else { // writeback miss (or RFO miss for L1D)
            
            DP ( if (warmup_complete[writeback_cpu]) {
            cout << "[" << NAME << "] " << __func__ << " type: " << +WQ.entry[index].type << " miss";
            cout << " instr_id: " << WQ.entry[index].instr_id << " address: " << hex << WQ.entry[index].address;
            cout << " full_addr: " << WQ.entry[index].full_addr << dec;
            cout << " cycle: " << WQ.entry[index].event_cycle << endl; });

			 {
                // find victim
                uint32_t set = get_set(WQ.entry[index].address), way;
                way = find_victim(writeback_cpu, WQ.entry[index].instr_id, set, block[set], WQ.entry[index].ip, WQ.entry[index].full_addr, WQ.entry[index].type);

                uint8_t  do_fill = 1;

                // is this dirty?
                if (block[set][way].state == M_STATE) {

                    // check if the lower level WQ has enough room to keep this writeback request
                    if (lower_level) { 
                        if (lower_level->get_occupancy(2, block[set][way].address) == lower_level->get_size(2, block[set][way].address)) {

                            // lower level WQ is full, cannot replace this victim
                            do_fill = 0;
                            lower_level->increment_WQ_FULL(block[set][way].address);
                            STALL[WQ.entry[index].type]++;

                            DP ( if (warmup_complete[writeback_cpu]) {
                            cout << "[" << NAME << "] " << __func__ << "do_fill: " << +do_fill;
                            cout << " lower level wq is full!" << " fill_addr: " << hex << WQ.entry[index].address;
                            cout << " victim_addr: " << block[set][way].tag << dec << endl; });
                        }
                        else { 
                            PACKET writeback_packet;

                            writeback_packet.fill_level = fill_level << 1;
                            writeback_packet.cpu = writeback_cpu;
                            writeback_packet.address = block[set][way].address;
                            writeback_packet.full_addr = block[set][way].full_addr;
                            writeback_packet.data = block[set][way].data;
                            writeback_packet.instr_id = WQ.entry[index].instr_id;
                            writeback_packet.ip = 0;
                            writeback_packet.type = WRITEBACK;
                            writeback_packet.event_cycle = current_core_cycle[writeback_cpu];

                            lower_level->add_wq(&writeback_packet);
                        }
                    }
#ifdef SANITY_CHECK
                    else {
                        // sanity check
                        if (cache_type != IS_STLB)
                            assert(0);
                    }
#endif
                }

                if (do_fill) {
                    // update replacement policy
                    update_replacement_state(writeback_cpu, set, way, WQ.entry[index].full_addr, WQ.entry[index].ip, block[set][way].full_addr, WQ.entry[index].type, 0);

                    // COLLECT STATS
                    sim_miss[writeback_cpu][WQ.entry[index].type]++;
                    sim_access[writeback_cpu][WQ.entry[index].type]++;

                    fill_cache(set, way, &WQ.entry[index]);

                    // mark dirty
                    //block[set][way].dirty = 1; 
		      block[set][way].state = M_STATE;

                    // check fill level
                    if (WQ.entry[index].fill_level < fill_level) {

                        if (WQ.entry[index].instruction) 
                            upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
                        else // data
                            upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
                    }

                    MISS[WQ.entry[index].type]++;
                    ACCESS[WQ.entry[index].type]++;

                    // remove this entry from WQ
                    WQ.remove_queue(&WQ.entry[index]);
                }
            }
        }
    }
}

void CACHE::handle_read()
{
    // handle read

    for (uint32_t i=0; i<MAX_READ; i++) {

      uint32_t read_cpu = RQ.entry[RQ.head].cpu;
      if (read_cpu == NUM_CPUS)
        return;

        // handle the oldest entry
        if ((RQ.entry[RQ.head].event_cycle <= current_core_cycle[read_cpu]) && (RQ.occupancy > 0)) {
            int index = RQ.head;

            // access cache
            uint32_t set = get_set(RQ.entry[index].address);
            int way = check_hit(&RQ.entry[index]);
            
            if (way >= 0) { // read hit

                if (cache_type == IS_ITLB) {
                    RQ.entry[index].instruction_pa = block[set][way].data;
                    if (PROCESSED.occupancy < PROCESSED.SIZE)
                        PROCESSED.add_queue(&RQ.entry[index]);
                }
                else if (cache_type == IS_DTLB) {
                    RQ.entry[index].data_pa = block[set][way].data;
                    if (PROCESSED.occupancy < PROCESSED.SIZE)
                        PROCESSED.add_queue(&RQ.entry[index]);
                }
                else if (cache_type == IS_STLB) 
                    RQ.entry[index].data = block[set][way].data;

                // update replacement policy
                update_replacement_state(read_cpu, set, way, block[set][way].full_addr, RQ.entry[index].ip, 0, RQ.entry[index].type, 1);

                // COLLECT STATS
                sim_hit[read_cpu][RQ.entry[index].type]++;
                sim_access[read_cpu][RQ.entry[index].type]++;

                // check fill level
                if (RQ.entry[index].fill_level < fill_level) {

                    if (RQ.entry[index].instruction) 
                        upper_level_icache[read_cpu]->return_data(&RQ.entry[index]);
                    else // data
                        upper_level_dcache[read_cpu]->return_data(&RQ.entry[index]);
                }

                // update prefetch stats and reset prefetch bit
                if (block[set][way].prefetch) {
                    pf_useful++;
                    block[set][way].prefetch = 0;
                }
                block[set][way].used = 1;

                HIT[RQ.entry[index].type]++;
                ACCESS[RQ.entry[index].type]++;
                
                // remove this entry from RQ
                RQ.remove_queue(&RQ.entry[index]);
		reads_available_this_cycle--;
            }
            else { // read miss

                DP ( if (warmup_complete[read_cpu]) {
                cout << "[" << NAME << "] " << __func__ << " read miss";
                cout << " instr_id: " << RQ.entry[index].instr_id << " address: " << hex << RQ.entry[index].address;
                cout << " full_addr: " << RQ.entry[index].full_addr << dec;
                cout << " cycle: " << RQ.entry[index].event_cycle << endl; });

                // check mshr
                uint8_t miss_handled = 1;
                int mshr_index = check_mshr(&RQ.entry[index]);

                if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE)) { // this is a new miss

				      // add it to mshr (read miss)
				      add_mshr(&RQ.entry[index]);
				      
				      // add it to the next level's read queue
				      if (lower_level)
		                        lower_level->add_rq(&RQ.entry[index]);
				      else { // this is the last level
		                        if (cache_type == IS_STLB) {
					  // TODO: need to differentiate page table walk and actual swap
					  
					  // emulate page table walk
					  uint64_t pa = va_to_pa(read_cpu, RQ.entry[index].instr_id, RQ.entry[index].full_addr, RQ.entry[index].address);
					  
					  RQ.entry[index].data = pa >> LOG2_PAGE_SIZE; 
					  RQ.entry[index].event_cycle = current_core_cycle[read_cpu];
					  return_data(&RQ.entry[index]);
		                        }
				      }
                }
                else {
                    if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE)) { // not enough MSHR resource
                        
                        // cannot handle miss request until one of MSHRs is available
                        miss_handled = 0;
                        STALL[RQ.entry[index].type]++;
                    }
                    else if (mshr_index != -1) { // already in-flight miss

                        // mark merged consumer
                        if (RQ.entry[index].type == RFO) {

                            if (RQ.entry[index].tlb_access) {
                                uint32_t sq_index = RQ.entry[index].sq_index;
                                MSHR.entry[mshr_index].store_merged = 1;
                                MSHR.entry[mshr_index].sq_index_depend_on_me.insert (sq_index);
				MSHR.entry[mshr_index].sq_index_depend_on_me.join (RQ.entry[index].sq_index_depend_on_me, SQ_SIZE);
                            }

                            if (RQ.entry[index].load_merged) {
                                //uint32_t lq_index = RQ.entry[index].lq_index; 
                                MSHR.entry[mshr_index].load_merged = 1;
                                //MSHR.entry[mshr_index].lq_index_depend_on_me[lq_index] = 1;
				MSHR.entry[mshr_index].lq_index_depend_on_me.join (RQ.entry[index].lq_index_depend_on_me, LQ_SIZE);
                            }
                        }
                        else {
                            if (RQ.entry[index].instruction) {
                                uint32_t rob_index = RQ.entry[index].rob_index;
                                MSHR.entry[mshr_index].instr_merged = 1;
                                MSHR.entry[mshr_index].rob_index_depend_on_me.insert (rob_index);

                                DP (if (warmup_complete[MSHR.entry[mshr_index].cpu]) {
                                cout << "[INSTR_MERGED] " << __func__ << " cpu: " << MSHR.entry[mshr_index].cpu << " instr_id: " << MSHR.entry[mshr_index].instr_id;
                                cout << " merged rob_index: " << rob_index << " instr_id: " << RQ.entry[index].instr_id << endl; });

                                if (RQ.entry[index].instr_merged) {
				    MSHR.entry[mshr_index].rob_index_depend_on_me.join (RQ.entry[index].rob_index_depend_on_me, ROB_SIZE);
                                    DP (if (warmup_complete[MSHR.entry[mshr_index].cpu]) {
                                    cout << "[INSTR_MERGED] " << __func__ << " cpu: " << MSHR.entry[mshr_index].cpu << " instr_id: " << MSHR.entry[mshr_index].instr_id;
                                    cout << " merged rob_index: " << i << " instr_id: N/A" << endl; });
                                }
                            }
                            else 
                            {
                                uint32_t lq_index = RQ.entry[index].lq_index;
                                MSHR.entry[mshr_index].load_merged = 1;
                                MSHR.entry[mshr_index].lq_index_depend_on_me.insert (lq_index);

                                DP (if (warmup_complete[read_cpu]) {
                                cout << "[DATA_MERGED] " << __func__ << " cpu: " << read_cpu << " instr_id: " << RQ.entry[index].instr_id;
                                cout << " merged rob_index: " << RQ.entry[index].rob_index << " instr_id: " << RQ.entry[index].instr_id << " lq_index: " << RQ.entry[index].lq_index << endl; });
				MSHR.entry[mshr_index].lq_index_depend_on_me.join (RQ.entry[index].lq_index_depend_on_me, LQ_SIZE);
                                if (RQ.entry[index].store_merged) {
                                    MSHR.entry[mshr_index].store_merged = 1;
				    MSHR.entry[mshr_index].sq_index_depend_on_me.join (RQ.entry[index].sq_index_depend_on_me, SQ_SIZE);
                                }
                            }
                        }

                        // update fill_level
                        if (RQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
                            MSHR.entry[mshr_index].fill_level = RQ.entry[index].fill_level;

                        // update request
                        if (MSHR.entry[mshr_index].type == PREFETCH) {
                            uint8_t  prior_returned = MSHR.entry[mshr_index].returned;
                            uint64_t prior_event_cycle = MSHR.entry[mshr_index].event_cycle;
                            MSHR.entry[mshr_index] = RQ.entry[index];
                            
                            // in case request is already returned, we should keep event_cycle and retunred variables
                            MSHR.entry[mshr_index].returned = prior_returned;
                            MSHR.entry[mshr_index].event_cycle = prior_event_cycle;
                        }

                        MSHR_MERGED[RQ.entry[index].type]++;

                        DP ( if (warmup_complete[read_cpu]) {
                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
                        cout << " instr_id: " << RQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                        cout << " address: " << hex << RQ.entry[index].address;
                        cout << " full_addr: " << RQ.entry[index].full_addr << dec;
                        cout << " cycle: " << RQ.entry[index].event_cycle << endl; });
                    }
                    else { // WE SHOULD NOT REACH HERE
                        cerr << "[" << NAME << "] MSHR errors" << endl;
                        assert(0);
                    }
                }

                if (miss_handled) {
                    // update prefetcher on load instruction

                    MISS[RQ.entry[index].type]++;
                    ACCESS[RQ.entry[index].type]++;

                    // remove this entry from RQ
                    RQ.remove_queue(&RQ.entry[index]);
		    reads_available_this_cycle--;
                }
            }
        }
	else
	  {
	    return;
	  }

	if(reads_available_this_cycle == 0)
	  {
	    return;
	  }
    }
}






void CACHE::handle_prefetch()
{
    // handle prefetch

    for (uint32_t i=0; i<MAX_READ; i++) {
      
      uint32_t prefetch_cpu = PQ.entry[PQ.head].cpu;
      if (prefetch_cpu == NUM_CPUS)
        return;

        // handle the oldest entry
        if ((PQ.entry[PQ.head].event_cycle <= current_core_cycle[prefetch_cpu]) && (PQ.occupancy > 0)) {
            int index = PQ.head;

            // access cache
            uint32_t set = get_set(PQ.entry[index].address);
            int way = check_hit(&PQ.entry[index]);
            
            if (way >= 0) { // prefetch hit

                // update replacement policy
                if (cache_type == IS_LLC) {
                    llc_update_replacement_state(prefetch_cpu, set, way, block[set][way].full_addr, PQ.entry[index].ip, 0, PQ.entry[index].type, 1);

                }
                else
                    update_replacement_state(prefetch_cpu, set, way, block[set][way].full_addr, PQ.entry[index].ip, 0, PQ.entry[index].type, 1);

                // COLLECT STATS
                sim_hit[prefetch_cpu][PQ.entry[index].type]++;
                sim_access[prefetch_cpu][PQ.entry[index].type]++;

		// run prefetcher on prefetches from higher caches
		if(PQ.entry[index].pf_origin_level < fill_level)
		  {
		    if (cache_type == IS_L1D)
		      l1d_prefetcher_operate(PQ.entry[index].full_addr, PQ.entry[index].ip, 1, PREFETCH);
                    else if (cache_type == IS_L2C)
                      PQ.entry[index].pf_metadata = l2c_prefetcher_operate(block[set][way].address<<LOG2_BLOCK_SIZE, PQ.entry[index].ip, 1, PREFETCH, PQ.entry[index].pf_metadata);
                    else if (cache_type == IS_LLC)
		      {
			cpu = prefetch_cpu;
			PQ.entry[index].pf_metadata = llc_prefetcher_operate(block[set][way].address<<LOG2_BLOCK_SIZE, PQ.entry[index].ip, 1, PREFETCH, PQ.entry[index].pf_metadata);
			cpu = 0;
		      }
		  }

                // check fill level
                if (PQ.entry[index].fill_level < fill_level) {

                    if (PQ.entry[index].instruction) 
                        upper_level_icache[prefetch_cpu]->return_data(&PQ.entry[index]);
                    else // data
                        upper_level_dcache[prefetch_cpu]->return_data(&PQ.entry[index]);
                }

                HIT[PQ.entry[index].type]++;
                ACCESS[PQ.entry[index].type]++;
                
                // remove this entry from PQ
                PQ.remove_queue(&PQ.entry[index]);
		reads_available_this_cycle--;
            }
            else { // prefetch miss

                DP ( if (warmup_complete[prefetch_cpu]) {
                cout << "[" << NAME << "] " << __func__ << " prefetch miss";
                cout << " instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
                cout << " full_addr: " << PQ.entry[index].full_addr << dec << " fill_level: " << PQ.entry[index].fill_level;
                cout << " cycle: " << PQ.entry[index].event_cycle << endl; });

                // check mshr
                uint8_t miss_handled = 1;
                int mshr_index = check_mshr(&PQ.entry[index]);

                if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE)) { // this is a new miss

                    DP ( if (warmup_complete[PQ.entry[index].cpu]) {
                    cout << "[" << NAME << "_PQ] " <<  __func__ << " want to add instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
                    cout << " full_addr: " << PQ.entry[index].full_addr << dec;
                    cout << " occupancy: " << lower_level->get_occupancy(3, PQ.entry[index].address) << " SIZE: " << lower_level->get_size(3, PQ.entry[index].address) << endl; });

                    // first check if the lower level PQ is full or not
                    // this is possible since multiple prefetchers can exist at each level of caches
                    if (lower_level) {
		      if (cache_type == IS_LLC) {
			if (lower_level->get_occupancy(1, PQ.entry[index].address) == lower_level->get_size(1, PQ.entry[index].address))
			  miss_handled = 0;
			else {
			  
			  // run prefetcher on prefetches from higher caches
			  if(PQ.entry[index].pf_origin_level < fill_level)
			    {
			      if (cache_type == IS_LLC)
				{
				  cpu = prefetch_cpu;
				  PQ.entry[index].pf_metadata = llc_prefetcher_operate(PQ.entry[index].address<<LOG2_BLOCK_SIZE, PQ.entry[index].ip, 0, PREFETCH, PQ.entry[index].pf_metadata);
				  cpu = 0;
				}
			    }
			  
			  // add it to MSHRs if this prefetch miss will be filled to this cache level
			  if (PQ.entry[index].fill_level <= fill_level)
			    add_mshr(&PQ.entry[index]);

			  lower_level->add_rq(&PQ.entry[index]); // add it to the DRAM RQ
			}
		      }
		      else {
			if (lower_level->get_occupancy(3, PQ.entry[index].address) == lower_level->get_size(3, PQ.entry[index].address))
			  miss_handled = 0;
			else {

			  // run prefetcher on prefetches from higher caches
			  if(PQ.entry[index].pf_origin_level < fill_level)
			    {
			      if (cache_type == IS_L1D)
				l1d_prefetcher_operate(PQ.entry[index].full_addr, PQ.entry[index].ip, 0, PREFETCH);
			      if (cache_type == IS_L2C)
				PQ.entry[index].pf_metadata = l2c_prefetcher_operate(PQ.entry[index].address<<LOG2_BLOCK_SIZE, PQ.entry[index].ip, 0, PREFETCH, PQ.entry[index].pf_metadata);
			    }
			  
			  // add it to MSHRs if this prefetch miss will be filled to this cache level
			  if (PQ.entry[index].fill_level <= fill_level)
			    add_mshr(&PQ.entry[index]);

			  lower_level->add_pq(&PQ.entry[index]); // add it to the DRAM RQ
			}
		      }
		    }
                }
                else {
                    if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE)) { // not enough MSHR resource

                        // TODO: should we allow prefetching with lower fill level at this case?
                        
                        // cannot handle miss request until one of MSHRs is available
                        miss_handled = 0;
                        STALL[PQ.entry[index].type]++;
                    }
                    else if (mshr_index != -1) { // already in-flight miss

                        // no need to update request except fill_level
                        // update fill_level
                        if (PQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
                            MSHR.entry[mshr_index].fill_level = PQ.entry[index].fill_level;

                        MSHR_MERGED[PQ.entry[index].type]++;

                        DP ( if (warmup_complete[prefetch_cpu]) {
                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
                        cout << " instr_id: " << PQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                        cout << " address: " << hex << PQ.entry[index].address;
                        cout << " full_addr: " << PQ.entry[index].full_addr << dec << " fill_level: " << MSHR.entry[mshr_index].fill_level;
                        cout << " cycle: " << MSHR.entry[mshr_index].event_cycle << endl; });
                    }
                    else { // WE SHOULD NOT REACH HERE
                        cerr << "[" << NAME << "] MSHR errors" << endl;
                        assert(0);
                    }
                }

                if (miss_handled) {

                    DP ( if (warmup_complete[prefetch_cpu]) {
                    cout << "[" << NAME << "] " << __func__ << " prefetch miss handled";
                    cout << " instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
                    cout << " full_addr: " << PQ.entry[index].full_addr << dec << " fill_level: " << PQ.entry[index].fill_level;
                    cout << " cycle: " << PQ.entry[index].event_cycle << endl; });

                    MISS[PQ.entry[index].type]++;
                    ACCESS[PQ.entry[index].type]++;

                    // remove this entry from PQ
                    PQ.remove_queue(&PQ.entry[index]);
		    reads_available_this_cycle--;
                }
            }
        }
	else
	  {
	    return;
	  }

	if(reads_available_this_cycle == 0)
	  {
	    return;
	  }
    }
}

void CACHE::operate()
{

	reads_available_this_cycle = MAX_READ;

	if(cache_type == IS_L1D || cache_type == IS_L1I)
	{
	    l1_handle_fill();
	    l1_handle_writeback();
	    l1_handle_read();
	}
	else if(cache_type == IS_L2C)
	{
	    l2_handle_response();
	    l2_handle_forwards();

	    l2_handle_writeback();
	    l2_handle_read();
	}
	else if(cache_type == IS_LLC)
	{
	    llc_handle_response();
	    llc_handle_request();	
	}
	else
	{
		handle_fill();
	    handle_writeback();
	    handle_read();
	}

    if (PQ.occupancy && (reads_available_this_cycle > 0))
        handle_prefetch();
}

uint32_t CACHE::get_set(uint64_t address)
{
    return (uint32_t) (address & ((1 << lg2(NUM_SET)) - 1)); 
}

uint32_t CACHE::get_way(uint64_t address, uint32_t set)
{
    for (uint32_t way=0; way<NUM_WAY; way++) {
        if (block[set][way].state != I_STATE && (block[set][way].tag == address)) 
            return way;
    }

    return NUM_WAY;
}

void CACHE::fill_cache(uint32_t set, uint32_t way, PACKET *packet)
{
#ifdef SANITY_CHECK
    if (cache_type == IS_ITLB) {
        if (packet->data == 0)
            assert(0);
    }

    if (cache_type == IS_DTLB) {
        if (packet->data == 0)
            assert(0);
    }

    if (cache_type == IS_STLB) {
        if (packet->data == 0)
            assert(0);
    }
#endif
    if (block[set][way].prefetch && (block[set][way].used == 0))
        pf_useless++;

    //if (block[set][way].valid == 0)
    //    block[set][way].valid = 1;
    //block[set][way].dirty = 0;
      block[set][way].state = S_STATE;

    block[set][way].prefetch = (packet->type == PREFETCH) ? 1 : 0;
    block[set][way].used = 0;

    if (block[set][way].prefetch)
        pf_fill++;

    block[set][way].delta = packet->delta;
    block[set][way].depth = packet->depth;
    block[set][way].signature = packet->signature;
    block[set][way].confidence = packet->confidence;

    block[set][way].tag = packet->address;
    block[set][way].address = packet->address;
    block[set][way].full_addr = packet->full_addr;
    block[set][way].data = packet->data;
    block[set][way].cpu = packet->cpu;
    block[set][way].instr_id = packet->instr_id;

    DP ( if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "] " << __func__ << " set: " << set << " way: " << way;
    cout << " lru: " << block[set][way].lru << " tag: " << hex << block[set][way].tag << " full_addr: " << block[set][way].full_addr;
    cout << " data: " << block[set][way].data << dec << endl; });
}

int CACHE::check_hit(PACKET *packet)
{
    uint32_t set = get_set(packet->address);
    int match_way = -1;

    if (NUM_SET < set) {
        cerr << "[" << NAME << "_ERROR] " << __func__ << " invalid set index: " << set << " NUM_SET: " << NUM_SET;
        cerr << " address: " << hex << packet->address << " full_addr: " << packet->full_addr << dec;
        cerr << " event: " << packet->event_cycle << endl;
        assert(0);
    }

    // hit
    for (uint32_t way=0; way<NUM_WAY; way++) {
        if (block[set][way].state != I_STATE && (block[set][way].tag == packet->address)) {

            match_way = way;

            DP ( if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "] " << __func__ << " instr_id: " << packet->instr_id << " type: " << +packet->type << hex << " addr: " << packet->address;
            cout << " full_addr: " << packet->full_addr << " tag: " << block[set][way].tag << " data: " << block[set][way].data << dec;
            cout << " set: " << set << " way: " << way << " lru: " << block[set][way].lru;
            cout << " event: " << packet->event_cycle << " cycle: " << current_core_cycle[cpu] << endl; });

            break;
        }
    }

    return match_way;
}

int CACHE::dir_check_hit(PACKET *packet)
{
    uint32_t set = get_set(packet->address);
    int match_way = -1;

    // hit
    for (uint32_t way=0; way<NUM_WAY; way++) {
        if (directory[set][way].state != I_STATE && (directory[set][way].tag == packet->address)) {

            match_way = way;
            break;
        }
    }

    return match_way;
}

int CACHE::invalidate_entry(uint64_t inval_addr)
{
    uint32_t set = get_set(inval_addr);
    int match_way = -1;

    if (NUM_SET < set) {
        cerr << "[" << NAME << "_ERROR] " << __func__ << " invalid set index: " << set << " NUM_SET: " << NUM_SET;
        cerr << " inval_addr: " << hex << inval_addr << dec << endl;
        assert(0);
    }

    // invalidate
    for (uint32_t way=0; way<NUM_WAY; way++) {
        if (block[set][way].state != I_STATE && (block[set][way].tag == inval_addr)) {

            block[set][way].state = I_STATE;

            match_way = way;

            DP ( if (warmup_complete[cpu]) {
            cout << "[" << NAME << "] " << __func__ << " inval_addr: " << hex << inval_addr;  
            cout << " tag: " << block[set][way].tag << " data: " << block[set][way].data << dec;
            cout << " set: " << set << " way: " << way << " lru: " << block[set][way].lru << " cycle: " << current_core_cycle[cpu] << endl; });

            break;
        }
    }

    return match_way;
}

int CACHE::add_rq(PACKET *packet)
{
    // check for the latest wirtebacks in the write queue
    int wq_index = WQ.check_queue(packet);
    if (wq_index != -1) {
        
        // check fill level
        if (packet->fill_level < fill_level) {

            packet->data = WQ.entry[wq_index].data;
            if (packet->instruction) 
                upper_level_icache[packet->cpu]->return_data(packet);
            else // data
                upper_level_dcache[packet->cpu]->return_data(packet);
        }

#ifdef SANITY_CHECK
        if (cache_type == IS_ITLB)
            assert(0);
        else if (cache_type == IS_DTLB)
            assert(0);
        else if (cache_type == IS_L1I)
            assert(0);
#endif
        // update processed packets
        if ((cache_type == IS_L1D) && (packet->type != PREFETCH)) {
            if (PROCESSED.occupancy < PROCESSED.SIZE)
                PROCESSED.add_queue(packet);

            DP ( if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "_RQ] " << __func__ << " instr_id: " << packet->instr_id << " found recent writebacks";
            cout << hex << " read: " << packet->address << " writeback: " << WQ.entry[wq_index].address << dec;
            cout << " index: " << MAX_READ << " rob_signal: " << packet->rob_signal << endl; });
        }

        HIT[packet->type]++;
        ACCESS[packet->type]++;

        WQ.FORWARD++;
        RQ.ACCESS++;

        return -1;
    }

    // check for duplicates in the read queue
    int index = RQ.check_queue(packet);
    if (index != -1) {
        
        if (packet->instruction) {
            uint32_t rob_index = packet->rob_index;
            RQ.entry[index].rob_index_depend_on_me.insert (rob_index);
            RQ.entry[index].instr_merged = 1;

            DP (if (warmup_complete[packet->cpu]) {
            cout << "[INSTR_MERGED] " << __func__ << " cpu: " << packet->cpu << " instr_id: " << RQ.entry[index].instr_id;
            cout << " merged rob_index: " << rob_index << " instr_id: " << packet->instr_id << endl; });
        }
        else 
        {
            // mark merged consumer
            if (packet->type == RFO) {

                uint32_t sq_index = packet->sq_index;
                RQ.entry[index].sq_index_depend_on_me.insert (sq_index);
                RQ.entry[index].store_merged = 1;
            }
            else {
                uint32_t lq_index = packet->lq_index; 
                RQ.entry[index].lq_index_depend_on_me.insert (lq_index);
                RQ.entry[index].load_merged = 1;

                DP (if (warmup_complete[packet->cpu]) {
                cout << "[DATA_MERGED] " << __func__ << " cpu: " << packet->cpu << " instr_id: " << RQ.entry[index].instr_id;
                cout << " merged rob_index: " << packet->rob_index << " instr_id: " << packet->instr_id << " lq_index: " << packet->lq_index << endl; });
            }
        }

        RQ.MERGED++;
        RQ.ACCESS++;

        return index; // merged index
    }

    // check occupancy
    if (RQ.occupancy == RQ_SIZE) {
        RQ.FULL++;

        return -2; // cannot handle this request
    }

    // if there is no duplicate, add it to RQ
    index = RQ.tail;

#ifdef SANITY_CHECK
    if (RQ.entry[index].address != 0) {
        cerr << "[" << NAME << "_ERROR] " << __func__ << " is not empty index: " << index;
        cerr << " address: " << hex << RQ.entry[index].address;
        cerr << " full_addr: " << RQ.entry[index].full_addr << dec << endl;
        assert(0);
    }
#endif

    RQ.entry[index] = *packet;

    // ADD LATENCY
    if (RQ.entry[index].event_cycle < current_core_cycle[packet->cpu])
        RQ.entry[index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
    else
        RQ.entry[index].event_cycle += LATENCY;

    RQ.occupancy++;
    RQ.tail++;
    if (RQ.tail >= RQ.SIZE)
        RQ.tail = 0;

    //DP ( if (warmup_complete[RQ.entry[index].cpu]) {
#ifdef PRINT_QUEUE_TRACE
    cout << "[" << NAME << "_RQ] " <<  __func__ << " instr_id: " << RQ.entry[index].instr_id << " address: " << hex << RQ.entry[index].address;
    cout << " full_addr: " << RQ.entry[index].full_addr << dec;
    cout << " type: " << +RQ.entry[index].type << " head: " << RQ.head << " tail: " << RQ.tail << " occupancy: " << RQ.occupancy;
    cout << " event: " << RQ.entry[index].event_cycle << " current: " << current_core_cycle[RQ.entry[index].cpu] << endl; //});
#endif

    if (packet->address == 0)
        assert(0);

    RQ.TO_CACHE++;
    RQ.ACCESS++;

    return -1;
}

int CACHE::add_wq(PACKET *packet)
{
    // check for duplicates in the write queue
    int index = WQ.check_queue(packet);
    if (index != -1) {

        WQ.MERGED++;
        WQ.ACCESS++;

        return index; // merged index
    }

    // sanity check
    if (WQ.occupancy >= WQ.SIZE)
        assert(0);

    // if there is no duplicate, add it to the write queue
    index = WQ.tail;
    if (WQ.entry[index].address != 0) {
        cerr << "[" << NAME << "_ERROR] " << __func__ << " is not empty index: " << index;
        cerr << " address: " << hex << WQ.entry[index].address;
        cerr << " full_addr: " << WQ.entry[index].full_addr << dec << endl;
        assert(0);
    }

    WQ.entry[index] = *packet;

    // ADD LATENCY
    if (WQ.entry[index].event_cycle < current_core_cycle[packet->cpu])
        WQ.entry[index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
    else
        WQ.entry[index].event_cycle += LATENCY;

    WQ.occupancy++;
    WQ.tail++;
    if (WQ.tail >= WQ.SIZE)
        WQ.tail = 0;

    //DP (if (warmup_complete[WQ.entry[index].cpu]) {
#ifdef PRINT_QUEUE_TRACE
    cout << "[" << NAME << "_WQ] " <<  __func__ << " instr_id: " << WQ.entry[index].instr_id << " address: " << hex << WQ.entry[index].address;
    cout << " full_addr: " << WQ.entry[index].full_addr << dec;
    cout << " head: " << WQ.head << " tail: " << WQ.tail << " occupancy: " << WQ.occupancy;
    cout << " data: " << hex << WQ.entry[index].data << dec;
    cout << " event: " << WQ.entry[index].event_cycle << " current: " << current_core_cycle[WQ.entry[index].cpu] << endl; //});
#endif

    WQ.TO_CACHE++;
    WQ.ACCESS++;

    return -1;
}

int CACHE::prefetch_line(uint64_t ip, uint64_t base_addr, uint64_t pf_addr, int pf_fill_level, uint32_t prefetch_metadata)
{
    pf_requested++;

    if (PQ.occupancy < PQ.SIZE) {
        if ((base_addr>>LOG2_PAGE_SIZE) == (pf_addr>>LOG2_PAGE_SIZE)) {
            
            PACKET pf_packet;
            pf_packet.fill_level = pf_fill_level;
	    pf_packet.pf_origin_level = fill_level;
	    pf_packet.pf_metadata = prefetch_metadata;
            pf_packet.cpu = cpu;
            //pf_packet.data_index = LQ.entry[lq_index].data_index;
            //pf_packet.lq_index = lq_index;
            pf_packet.address = pf_addr >> LOG2_BLOCK_SIZE;
            pf_packet.full_addr = pf_addr;
            //pf_packet.instr_id = LQ.entry[lq_index].instr_id;
            //pf_packet.rob_index = LQ.entry[lq_index].rob_index;
            pf_packet.ip = ip;
            pf_packet.type = PREFETCH;
            pf_packet.event_cycle = current_core_cycle[cpu];

            // give a dummy 0 as the IP of a prefetch
            add_pq(&pf_packet);

            pf_issued++;

            return 1;
        }
    }

    return 0;
}

int CACHE::kpc_prefetch_line(uint64_t base_addr, uint64_t pf_addr, int pf_fill_level, int delta, int depth, int signature, int confidence, uint32_t prefetch_metadata)
{
    if (PQ.occupancy < PQ.SIZE) {
        if ((base_addr>>LOG2_PAGE_SIZE) == (pf_addr>>LOG2_PAGE_SIZE)) {
            
            PACKET pf_packet;
            pf_packet.fill_level = pf_fill_level;
	    pf_packet.pf_origin_level = fill_level;
	    pf_packet.pf_metadata = prefetch_metadata;
            pf_packet.cpu = cpu;
            //pf_packet.data_index = LQ.entry[lq_index].data_index;
            //pf_packet.lq_index = lq_index;
            pf_packet.address = pf_addr >> LOG2_BLOCK_SIZE;
            pf_packet.full_addr = pf_addr;
            //pf_packet.instr_id = LQ.entry[lq_index].instr_id;
            //pf_packet.rob_index = LQ.entry[lq_index].rob_index;
            pf_packet.ip = 0;
            pf_packet.type = PREFETCH;
            pf_packet.delta = delta;
            pf_packet.depth = depth;
            pf_packet.signature = signature;
            pf_packet.confidence = confidence;
            pf_packet.event_cycle = current_core_cycle[cpu];

            // give a dummy 0 as the IP of a prefetch
            add_pq(&pf_packet);

            pf_issued++;

            return 1;
        }
    }

    return 0;
}

int CACHE::add_pq(PACKET *packet)
{
    // check for the latest wirtebacks in the write queue
    int wq_index = WQ.check_queue(packet);
    if (wq_index != -1) {
        
        // check fill level
        if (packet->fill_level < fill_level) {

            packet->data = WQ.entry[wq_index].data;
            if (packet->instruction) 
                upper_level_icache[packet->cpu]->return_data(packet);
            else // data
                upper_level_dcache[packet->cpu]->return_data(packet);
        }

        HIT[packet->type]++;
        ACCESS[packet->type]++;

        WQ.FORWARD++;
        PQ.ACCESS++;

        return -1;
    }

    // check for duplicates in the PQ
    int index = PQ.check_queue(packet);
    if (index != -1) {
        if (packet->fill_level < PQ.entry[index].fill_level)
            PQ.entry[index].fill_level = packet->fill_level;

        PQ.MERGED++;
        PQ.ACCESS++;

        return index; // merged index
    }

    // check occupancy
    if (PQ.occupancy == PQ_SIZE) {
        PQ.FULL++;

        DP ( if (warmup_complete[packet->cpu]) {
        cout << "[" << NAME << "] cannot process add_pq since it is full" << endl; });
        return -2; // cannot handle this request
    }

    // if there is no duplicate, add it to PQ
    index = PQ.tail;

#ifdef SANITY_CHECK
    if (PQ.entry[index].address != 0) {
        cerr << "[" << NAME << "_ERROR] " << __func__ << " is not empty index: " << index;
        cerr << " address: " << hex << PQ.entry[index].address;
        cerr << " full_addr: " << PQ.entry[index].full_addr << dec << endl;
        assert(0);
    }
#endif

    PQ.entry[index] = *packet;

    // ADD LATENCY
    if (PQ.entry[index].event_cycle < current_core_cycle[packet->cpu])
        PQ.entry[index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
    else
        PQ.entry[index].event_cycle += LATENCY;

    PQ.occupancy++;
    PQ.tail++;
    if (PQ.tail >= PQ.SIZE)
        PQ.tail = 0;

    DP ( if (warmup_complete[PQ.entry[index].cpu]) {
    cout << "[" << NAME << "_PQ] " <<  __func__ << " instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
    cout << " full_addr: " << PQ.entry[index].full_addr << dec;
    cout << " type: " << +PQ.entry[index].type << " head: " << PQ.head << " tail: " << PQ.tail << " occupancy: " << PQ.occupancy;
    cout << " event: " << PQ.entry[index].event_cycle << " current: " << current_core_cycle[PQ.entry[index].cpu] << endl; });

    if (packet->address == 0)
        assert(0);

    PQ.TO_CACHE++;
    PQ.ACCESS++;

    return -1;
}

void CACHE::return_data(PACKET *packet)
{
    // check MSHR information
    int mshr_index = check_mshr(packet);

    // sanity check
    if (mshr_index == -1) {
        cerr << "[" << NAME << "_MSHR] " << __func__ << " instr_id: " << packet->instr_id << " cannot find a matching entry!";
        cerr << " full_addr: " << hex << packet->full_addr;
        cerr << " address: " << packet->address << dec;
        cerr << " event: " << packet->event_cycle << " current: " << current_core_cycle[packet->cpu] << endl;
        assert(0);
    }

    // MSHR holds the most updated information about this request
    // no need to do memcpy
    MSHR.num_returned++;
    MSHR.entry[mshr_index].returned = COMPLETED;
    MSHR.entry[mshr_index].data = packet->data;
    MSHR.entry[mshr_index].pf_metadata = packet->pf_metadata;

    // ADD LATENCY
    if (MSHR.entry[mshr_index].event_cycle < current_core_cycle[packet->cpu])
        MSHR.entry[mshr_index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
    else
        MSHR.entry[mshr_index].event_cycle += LATENCY;

    update_fill_cycle();

    DP (if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "_MSHR] " <<  __func__ << " instr_id: " << MSHR.entry[mshr_index].instr_id;
    cout << " address: " << hex << MSHR.entry[mshr_index].address << " full_addr: " << MSHR.entry[mshr_index].full_addr;
    cout << " data: " << MSHR.entry[mshr_index].data << dec << " num_returned: " << MSHR.num_returned;
    cout << " index: " << mshr_index << " occupancy: " << MSHR.occupancy;
    cout << " event: " << MSHR.entry[mshr_index].event_cycle << " current: " << current_core_cycle[packet->cpu] << " next: " << MSHR.next_fill_cycle << endl; });
}

void CACHE::update_fill_cycle()
{
    // update next_fill_cycle
    uint64_t min_cycle = UINT64_MAX;
    uint32_t min_index = MSHR.SIZE;
    for (uint32_t i=0; i<MSHR.SIZE; i++) {
        if ((MSHR.entry[i].returned == COMPLETED) && (MSHR.entry[i].event_cycle < min_cycle)) {
            min_cycle = MSHR.entry[i].event_cycle;
            min_index = i;
        }

        DP (if (warmup_complete[MSHR.entry[i].cpu]) {
        cout << "[" << NAME << "_MSHR] " <<  __func__ << " checking instr_id: " << MSHR.entry[i].instr_id;
        cout << " address: " << hex << MSHR.entry[i].address << " full_addr: " << MSHR.entry[i].full_addr;
        cout << " data: " << MSHR.entry[i].data << dec << " returned: " << +MSHR.entry[i].returned << " fill_level: " << MSHR.entry[i].fill_level;
        cout << " index: " << i << " occupancy: " << MSHR.occupancy;
        cout << " event: " << MSHR.entry[i].event_cycle << " current: " << current_core_cycle[MSHR.entry[i].cpu] << " next: " << MSHR.next_fill_cycle << endl; });
    }
    
    MSHR.next_fill_cycle = min_cycle;
    MSHR.next_fill_index = min_index;
    if (min_index < MSHR.SIZE) {

        DP (if (warmup_complete[MSHR.entry[min_index].cpu]) {
        cout << "[" << NAME << "_MSHR] " <<  __func__ << " instr_id: " << MSHR.entry[min_index].instr_id;
        cout << " address: " << hex << MSHR.entry[min_index].address << " full_addr: " << MSHR.entry[min_index].full_addr;
        cout << " data: " << MSHR.entry[min_index].data << dec << " num_returned: " << MSHR.num_returned;
        cout << " event: " << MSHR.entry[min_index].event_cycle << " current: " << current_core_cycle[MSHR.entry[min_index].cpu] << " next: " << MSHR.next_fill_cycle << endl; });
    }
}

int CACHE::check_mshr(PACKET *packet)
{
    // search mshr
    for (uint32_t index=0; index<MSHR_SIZE; index++) {
        if (MSHR.entry[index].address == packet->address) {
            
            DP ( if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "_MSHR] " << __func__ << " same entry instr_id: " << packet->instr_id << " prior_id: " << MSHR.entry[index].instr_id;
            cout << " address: " << hex << packet->address;
            cout << " full_addr: " << packet->full_addr << dec << endl; });

            return index;
        }
    }

    DP ( if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "_MSHR] " << __func__ << " new address: " << hex << packet->address;
    cout << " full_addr: " << packet->full_addr << dec << endl; });

    DP ( if (warmup_complete[packet->cpu] && (MSHR.occupancy == MSHR_SIZE)) { 
    cout << "[" << NAME << "_MSHR] " << __func__ << " mshr is full";
    cout << " instr_id: " << packet->instr_id << " mshr occupancy: " << MSHR.occupancy;
    cout << " address: " << hex << packet->address;
    cout << " full_addr: " << packet->full_addr << dec;
    cout << " cycle: " << current_core_cycle[packet->cpu] << endl; });

    return -1;
}

void CACHE::add_mshr(PACKET *packet)
{
    uint32_t index = 0;

    packet->cycle_enqueued = current_core_cycle[packet->cpu];

    // search mshr
    for (index=0; index<MSHR_SIZE; index++) {
        if (MSHR.entry[index].address == 0) {
            
            MSHR.entry[index] = *packet;
            MSHR.entry[index].returned = INFLIGHT;
            MSHR.occupancy++;

            //DP ( if (warmup_complete[packet->cpu]) {
#ifdef PRINT_QUEUE_TRACE
	    cout << "[" << NAME << "_MSHR] " << __func__ << " instr_id: " << packet->instr_id;
            cout << " address: " << hex << packet->address << " full_addr: " << packet->full_addr << dec;
            cout << " index: " << index << " occupancy: " << MSHR.occupancy << endl; //});
#endif

            break;
        }
    }
}

int CACHE::check_fab(PACKET *packet)
{
	assert(cache_type == IS_L2C);
    // search mshr
    for (uint32_t index=0; index<L2C_FAB_SIZE; index++) {
        if (FAB.entry[index].address == packet->address) {
            
            return index;
        }
    }

    return -1;
}

void CACHE::add_fab(PACKET *packet)
{
	assert(cache_type == IS_L2C);
    uint32_t index = 0;

    // search FAB
    for (index=0; index<MSHR_SIZE; index++) {
        if (FAB.entry[index].address == 0) {
            
            FAB.entry[index] = *packet;
            FAB.occupancy++;

            break;
        }
    }
}

uint32_t CACHE::get_occupancy(uint8_t queue_type, uint64_t address)
{
    if (queue_type == 0)
        return MSHR.occupancy;
    else if (queue_type == 1)
        return RQ.occupancy;
    else if (queue_type == 2)
        return WQ.occupancy;
    else if (queue_type == 3)
        return PQ.occupancy;
    else if (queue_type == 4)
        return REQQ.occupancy;
    else if (queue_type == 5)
        return RESQ.occupancy;
    else if (queue_type == 6)
        return FWQ.occupancy;

    return 0;
}

uint32_t CACHE::get_size(uint8_t queue_type, uint64_t address)
{
    if (queue_type == 0)
        return MSHR.SIZE;
    else if (queue_type == 1)
        return RQ.SIZE;
    else if (queue_type == 2)
        return WQ.SIZE;
    else if (queue_type == 3)
        return PQ.SIZE;
    else if (queue_type == 4)
        return REQQ.SIZE;
    else if (queue_type == 5)
        return RESQ.SIZE;
    else if (queue_type == 6)
        return FWQ.SIZE;

    return 0;
}

void CACHE::increment_WQ_FULL(uint64_t address)
{
    WQ.FULL++;
}
