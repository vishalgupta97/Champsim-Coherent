/*BEGIN_LEGAL 
Intel Open Source License 

Copyright (c) 2002-2018 Intel Corporation. All rights reserved.
 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.  Redistributions
in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.  Neither the name of
the Intel Corporation nor the names of its contributors may be used to
endorse or promote products derived from this software without
specific prior written permission.
 
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL OR
ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */
/*
 *  This file contains an ISA-portable PIN tool for tracing memory accesses.
 */

#include <stdio.h>
#include "pin.H"
#include <iostream>
#include <iterator> 
#include <map> 
#include <set>
#include <fstream>
using namespace std;

FILE * trace;
PIN_LOCK pinLock;
unsigned long machine_access = 0;
unsigned long bd_end = 0;
unsigned long long total_access_distance = 0;
map <unsigned long long, unsigned long long > block_distance;
map <unsigned long long, unsigned long long> cumulative_distance;
set<unsigned long long> unique_addr;

// Print a memory read record
VOID RecordMachineAccess(VOID * addr, UINT32 size, THREADID tid)
{
	PIN_GetLock(&pinLock, tid+1);
    unsigned long long a = (unsigned long long)(addr);
    unsigned long long curr_a = a;
    //fprintf(trace,"Address: %ld Size: %d Thread id: %d\n",a,size,tid);
	curr_a = a >> 6;
	//printf("%ld %d\n",curr_a,size);
	unique_addr.insert(curr_a);


    PIN_ReleaseLock(&pinLock);
}
VOID RecordMemRead(VOID * ip, VOID * addr,UINT32 size, THREADID tid)
{
	RecordMachineAccess(addr,size,tid);
}

// Print a memory write record
VOID RecordMemWrite(VOID * ip, VOID * addr, UINT32 size, THREADID tid)
{
    RecordMachineAccess(addr,size,tid);
}

// Is called for every instruction and instruments reads and writes
VOID Instruction(INS ins, VOID *v)
{
    // Instruments memory accesses using a predicated call, i.e.
    // the instrumentation is called iff the instruction will actually be executed.
    //
    // On the IA-32 and Intel(R) 64 architectures conditional moves and REP 
    // prefixed instructions appear as predicated instructions in Pin.
    UINT32 memOperands = INS_MemoryOperandCount(ins);

    // Iterate over each memory operand of the instruction.
    for (UINT32 memOp = 0; memOp < memOperands; memOp++)
    {
        if (INS_MemoryOperandIsRead(ins, memOp))
        {
		//	if(INS_hasKnownMemorySize(ins))
			
            	INS_InsertPredicatedCall(
                	ins, IPOINT_BEFORE, (AFUNPTR)RecordMemRead,
                	IARG_INST_PTR,
                	IARG_MEMORYOP_EA, memOp, IARG_MEMORYREAD_SIZE,
					IARG_THREAD_ID,
                	IARG_END);
			
        }
        // Note that in some architectures a single memory operand can be 
        // both read and written (for instance incl (%eax) on IA-32)
        // In that case we instrument it once for read and once for write.
        if (INS_MemoryOperandIsWritten(ins, memOp))
        {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE, (AFUNPTR)RecordMemWrite,
                IARG_INST_PTR,
                IARG_MEMORYOP_EA, memOp, IARG_MEMORYWRITE_SIZE,
				IARG_THREAD_ID,
                IARG_END);
        }
    }
}

VOID Fini(INT32 code, VOID *v)
{
	//unsigned long count=0;
	for (auto it=unique_addr.begin(); it != unique_addr.end(); ++it){
        //cout << ' ' << *it<<endl;
		fprintf(trace,"%llu \n",*it);

		//count++;
	}
	//cout<<"Count"<<count<<endl;
    fclose(trace);
}

/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */

int main(int argc, char *argv[])
{
    //if (PIN_Init(argc, argv)) return Usage();
PIN_Init(argc, argv);
    trace = fopen("unique_address.out", "w");
	PIN_InitLock(&pinLock);

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);
	
    // Never returns
    PIN_StartProgram();

    return 0;
}


