/*
// File homework1.cpp
//
// Author: Michal Stec
//         326777
//
//
//
*/

#include "aca2009.h"
#include <systemc.h>
#include <iostream>

#include "MEMORY.hpp"
#include "CPU.hpp"

using namespace std;

int sc_main(int argc, char** argv)
{
  // Open VCD file
  sc_trace_file *wf = sc_create_vcd_trace_file("homework1");

    try
    {
        // Get the tracefile argument and create Tracefile object
        // This function sets tracefile_ptr and num_cpus
        init_tracefile(&argc, &argv);

        // Initialize statistics counters
        stats_init();

        // Instantiate Modules
        Memory  mem("cache");
        CPU     cpu("cpu");

        // Signals
        sc_buffer<Memory::Function>   sigMemFunc;
        sc_buffer<Memory::RetCode>    sigMemDone;
        sc_signal<unsigned>           sigMemAddr;
        sc_signal_rv<32>              sigMemData;

        // The clock that iwill drive the CPU and Memory
        sc_clock clk;


        // Connecting module ports with signals
        mem.Port_Func(sigMemFunc);
        mem.Port_Addr(sigMemAddr);
        mem.Port_Data(sigMemData);
        mem.Port_Done(sigMemDone);

        cpu.Port_MemFunc(sigMemFunc);
        cpu.Port_MemAddr(sigMemAddr);
        cpu.Port_MemData(sigMemData);
        cpu.Port_MemDone(sigMemDone);

        mem.Port_CLK(clk);
        cpu.Port_CLK(clk);

        // Dump the desired signals
        sc_trace(wf, clk, "clock");
        sc_trace(wf, sigMemFunc, "function");// [0: read, 1: write]");
        sc_trace(wf, sigMemDone, "done");// [0: read, 1: write]");



        cout << "Running (press CTRL+C to interrupt) ..." << endl;

        // Start Simulation
        sc_start();

        // Print statistics after simulation finished
        stats_print();
    }
    catch(exception& e)
    {
      cerr << e.what() << endl;
    }

    sc_close_vcd_trace_file(wf);
    return 0;
}
