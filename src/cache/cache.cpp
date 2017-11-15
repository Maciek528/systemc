#include "aca2009.h"
#include <systemc.h>
#include <iostream>
#include <list>

using namespace std;

// static const int MEM_SIZE = 32768;
static const int MEM_SIZE = 512;

static const int NUM_SETS = 128;
static const int NUM_LINES = 8;
static const int SET_SIZE = 256;
static const int LRU_POSITION = NUM_LINES - 1;
static const int MRU_POSITION = 0;

SC_MODULE(Memory)
{

public:
    enum Function
    {
        FUNC_READ,
        FUNC_WRITE
    };

    enum RetCode
    {
        RET_READ_DONE,
        RET_WRITE_DONE,
    };

    sc_in<bool>     Port_CLK;
    sc_in<Function> Port_Func;
    sc_in<int>      Port_Addr;
    sc_out<RetCode> Port_Done;
    sc_inout_rv<32> Port_Data;

    SC_CTOR(Memory)
    {
        SC_THREAD(execute);
        sensitive << Port_CLK.pos();
        dont_initialize();

        m_data = new int[MEM_SIZE];
    }

    ~Memory()
    {
        delete[] m_data;
    }

private:
    int* m_data;

    void execute()
    {
        while (true)
        {
            wait(Port_Func.value_changed_event());	// this is fine since we use sc_buffer

            Function f = Port_Func.read();
            int addr   = Port_Addr.read();
            int data   = 0;
            if (f == FUNC_WRITE)
            {
                cout << sc_time_stamp() << ": MEM received write" << endl;
                data = Port_Data.read().to_int();
            }
            else
            {
                cout << sc_time_stamp() << ": MEM received read" << endl;
            }

            // This simulates memory read/write delay
            wait(99);

            if (f == FUNC_READ)
            {
                Port_Data.write( (addr < MEM_SIZE) ? m_data[addr] : 0 );
                Port_Done.write( RET_READ_DONE );
                wait();
                Port_Data.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
            }
            else
            {
                if (addr < MEM_SIZE)
                {
                    m_data[addr] = data;
                }
                Port_Done.write( RET_WRITE_DONE );
            }
        }
    }
};



SC_MODULE(Cache)
{

public:
    enum Function
    {
        FUNC_READ,
        FUNC_WRITE
    };

    enum RetCode
    {
        RET_READ_DONE,
        RET_WRITE_DONE,
    };

    sc_in<bool>       Port_CLK;
    sc_in<Function>   Port_CpuFunc;
    sc_in<int>        Port_CpuAddr;
    sc_in<RetCode>    Port_MemDone;

    sc_out<Function>  Port_MemFunc;
    sc_out<int>       Port_MemAddr;
    sc_out<RetCode>   Port_CpuDone;

    sc_inout_rv<32>   Port_CpuData;
    sc_inout_rv<32>   Port_MemData;

    class Line {
      public:
        bool  isValid;
        int   tag;
        int   data;

        Line() {
          isValid = false;
          tag = 0;
          data = 0;
        }
    };

    class Set {
      public:
        bool linePresent;
        Line line[NUM_LINES];
        Set (){
          linePresent = false;
        };

        int findTag (int tag){
          for(int i=0 ; i<NUM_LINES ; i++){
            if (line[i].tag == tag){
              return i;
            }
          }
          return -1;
        }

        // Shift all lines right up to the linePosition.
        // If linePosition is LRU_POSITION then the entry is removed.
        void shiftLines(int linePosition) {
          for(int i = 0 ; i < linePosition ; i++) {
            line[i+1] = line[i];
          }
        }
    };

    SC_CTOR(Cache)
    {
        Set set[NUM_SETS];
        for (int i=0 ; i<NUM_SETS ; i++){
          for (int j=0 ; j < NUM_LINES ; j++){
            Line newLine;
            set[i].line[j] = newLine;
          }
        }

        SC_THREAD(execute);
        sensitive << Port_CLK.pos();
        dont_initialize();
    }

private:
    Set* set;
    // Memory::Function  f;

    int getIndex (int address) {
      return address & 0xFE0;
    }

    int getTag (int address) {
      return address & 0xFFFFF000;
    }

    void execute()
    {
        while (true)
        {
            wait(Port_CpuFunc.value_changed_event());	// this is fine since we use sc_buffer

            Function f = Port_CpuFunc.read();
            int addr   = Port_CpuAddr.read();
            Port_MemAddr.write(addr);
            Port_MemFunc.write(f);

            int index  = getIndex(addr);
            int tag    = getTag(addr);
            int data   = 0;

            Set currentSet = set[index];

            // cout << set[index].linePresent << endl;

            int linePosition = currentSet.findTag(tag);

            if (linePosition > -1)
            currentSet.linePresent = true;

            Port_MemAddr.write(addr);
            Port_MemFunc.write(f);

            if (f == FUNC_WRITE)
            {
                cout << sc_time_stamp() << ": CACHE received write" << endl;
                data = Port_CpuData.read().to_int();
            }
            else
            {
                cout << sc_time_stamp() << ": CACHE received read" << endl;
            }

            // This simulates memory read/write delay
            wait(99);

            if (f == FUNC_READ)
            {

              //cache read hit
              if(currentSet.linePresent) {
                Line temp = currentSet.line[linePosition];
                currentSet.shiftLines(linePosition);
                currentSet.line[MRU_POSITION] = temp;
              }
              // cache read miss
              else {
                data = Port_MemData.read().to_int();
                currentSet.shiftLines(LRU_POSITION);
                currentSet.line[MRU_POSITION].data = data;
                currentSet.line[MRU_POSITION].isValid = true;
              }

                Port_CpuDone.write( RET_READ_DONE );
                wait();
                Port_CpuData.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
            }
            else //writing
            {
                // cache write hit
                if (currentSet.linePresent) {
                  ;
                }
                // cache write miss
                else {
                  if (currentSet.line[LRU_POSITION].isValid) {
                    Port_MemData.write(data);
                    wait();
                    Port_MemData.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
                  }

                  currentSet.shiftLines(LRU_POSITION);
                  currentSet.line[MRU_POSITION].data = data;
                  currentSet.line[MRU_POSITION].isValid = true;
                }

                Port_CpuDone.write( RET_WRITE_DONE );
            }
        }
    }
};

SC_MODULE(CPU)
{

public:
    sc_in<bool>                 Port_CLK;
    sc_in<Cache::RetCode>       Port_CacheDone;
    sc_out<Cache::Function>     Port_CacheFunc;
    sc_out<int>                 Port_CacheAddr;
    sc_inout_rv<32>             Port_CacheData;

    SC_CTOR(CPU)
    {
        SC_THREAD(execute);
        sensitive << Port_CLK.pos();
        dont_initialize();
    }

private:
    void execute()
    {
        TraceFile::Entry    tr_data;
        Cache::Function  f;

        // Loop until end of tracefile
        while(!tracefile_ptr->eof())
        {
            // Get the next action for the processor in the trace
            if(!tracefile_ptr->next(0, tr_data))
            {
                cerr << "Error reading trace for CPU" << endl;
                break;
            }

            // To demonstrate the statistic functions, we generate a 50%
            // probability of a 'hit' or 'miss', and call the statistic
            // functions below
            int j = rand()%2;

            switch(tr_data.type)
            {
                case TraceFile::ENTRY_TYPE_READ:
                    f = Cache::FUNC_READ;
                    if(j)
                        stats_readhit(0);
                    else
                        stats_readmiss(0);
                    break;

                case TraceFile::ENTRY_TYPE_WRITE:
                    f = Cache::FUNC_WRITE;
                    if(j)
                        stats_writehit(0);
                    else
                        stats_writemiss(0);
                    break;

                case TraceFile::ENTRY_TYPE_NOP:
                    break;

                default:
                    cerr << "Error, got invalid data from Trace" << endl;
                    exit(0);
            }

            if(tr_data.type != TraceFile::ENTRY_TYPE_NOP)
            {
                Port_CacheAddr.write(tr_data.addr);
                Port_CacheFunc.write(f);

                if (f == Cache::FUNC_WRITE)
                {
                    cout << sc_time_stamp() << ": CPU sends write" << endl;

                    uint32_t data = rand();
                    Port_CacheData.write(data);
                    wait();
                    Port_CacheData.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
                }
                else
                {
                    cout << sc_time_stamp() << ": CPU sends read" << endl;
                }

                wait(Port_CacheDone.value_changed_event());

                if (f == Cache::FUNC_READ)
                {
                    cout << sc_time_stamp() << ": CPU reads: " << Port_CacheData.read() << endl;
                }
            }
            else
            {
                cout << sc_time_stamp() << ": CPU executes NOP" << endl;
            }
            // Advance one cycle in simulated time
            wait();
        }

        // Finished the Tracefile, now stop the simulation
        sc_stop();
    }
};


int sc_main(int argc, char* argv[])
{
    try
    {
        // Get the tracefile argument and create Tracefile object
        // This function sets tracefile_ptr and num_cpus
        init_tracefile(&argc, &argv);

        // Initialize statistics counters
        stats_init();

        // Instantiate Modules
        Cache  cache("cache");
        Memory mem("main_memory");
        CPU    cpu("cpu");

        // Signals

        // signals cpu <=> cache
        sc_buffer<Cache::Function>  sigCpuFunc;
        sc_buffer<Cache::RetCode>   sigCpuDone;
        sc_signal<int>              sigCpuAddr;
        sc_signal_rv<32>            sigCpuData;

        //signals cache <=> mem
        sc_buffer<Memory::Function> sigMemFunc;
        sc_buffer<Memory::RetCode>  sigMemDone;
        sc_signal<int>              sigMemAddr;
        sc_signal_rv<32>            sigMemData;

        // The clock that will drive the CPU, Cache and Memory
        sc_clock clk;

        // Connecting module ports with signals

        mem.Port_Func(sigMemFunc);
        mem.Port_Addr(sigMemAddr);
        mem.Port_Data(sigMemData);
        mem.Port_Done(sigMemDone);

        cache.Port_MemFunc(sigMemFunc);
        cache.Port_MemAddr(sigMemAddr);
        cache.Port_MemData(sigMemData);
        cache.Port_MemDone(sigMemDone);

        cache.Port_CpuFunc(sigCpuFunc);
        cache.Port_CpuAddr(sigCpuAddr);
        cache.Port_CpuData(sigCpuData);
        cache.Port_CpuDone(sigCpuDone);

        cpu.Port_CacheFunc(sigCpuFunc);
        cpu.Port_CacheAddr(sigCpuAddr);
        cpu.Port_CacheData(sigCpuData);
        cpu.Port_CacheDone(sigCpuDone);

        cache.Port_CLK(clk);
        mem.Port_CLK(clk);
        cpu.Port_CLK(clk);

        cout << "Running (press CTRL+C to interrupt)... " << endl;


        // Start Simulation
        sc_start();

        // Print statistics after simulation finished
        stats_print();
    }

    catch (exception& e)
    {
        cerr << e.what() << endl;
    }

    return 0;
}
