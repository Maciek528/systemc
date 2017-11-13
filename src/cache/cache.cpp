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
    sc_in<Function>   Port_Func_Cpu;
    sc_in<int>        Port_Addr_Cpu;
    sc_in<RetCode>    Port_Done_Mem;
    sc_out<Function>  Port_Func_Mem;
    sc_out<int>       Port_Addr_Mem;
    sc_out<RetCode>   Port_Done_Cpu;
    sc_inout_rv<32>   Port_Data;

    typedef struct {
      bool  valid;
      int   tag;
      int   data;
    } Cache_Line;

    SC_CTOR(Cache)
    {
        SC_THREAD(execute);
        sensitive << Port_CLK.pos();
        dont_initialize();

        //construct cache
    }

    // ~Cache()
    // {
    //     delete[] cache_data;
    // }

private:
    // int* cache_data;

    int getIndex (int address) {
      return address & 0xFE0;
    }

    int getOffset (int address) {
      return address & 0xF1;
    }

    int getTag (int address) {
      return address & 0xFFFFF000;
    }

    void execute()
    {
        while (true)
        {
            wait(Port_Func_Cpu.value_changed_event());	// this is fine since we use sc_buffer

            Function f = Port_Func_Cpu.read();
            int addr   = Port_Addr_Cpu.read();
            int index  = getIndex(addr);
            int offset = getOffset(addr);
            int tag    = getTag(addr);
            int data   = 0;

            // currentSet = cache_data

            if (f == FUNC_WRITE)
            {
                cout << sc_time_stamp() << ": CACHE received write" << endl;
                cout << "to address: " << addr << endl;
                data = Port_Data.read().to_int();
                cout << "data: " << data << endl;
            }
            else
            {
                cout << sc_time_stamp() << ": CACHE received read" << endl;
                cout << "from address: " << addr << endl;
            }

            // This simulates memory read/write delay
            wait(99);

            if (f == FUNC_READ)
            {

                // Port_Data.write( (addr < MEM_SIZE) ? cache_data[addr] : 0 );
                Port_Done_Cpu.write( RET_READ_DONE );
                wait();
                Port_Data.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
            }
            else
            {
                if (addr < MEM_SIZE)
                {
                    // c_data[addr] = data;
                }
                Port_Done_Cpu.write( RET_WRITE_DONE );
            }
        }
    }
};

SC_MODULE(CPU)
{

public:
    sc_in<bool>                 Port_CLK;
    sc_in<Cache::RetCode>       Port_Done;
    sc_out<Cache::Function>     Port_Func;
    sc_out<int>                 Port_Addr;
    sc_inout_rv<32>             Port_Data;

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
                Port_Addr.write(tr_data.addr);
                Port_Func.write(f);

                if (f == Cache::FUNC_WRITE)
                {
                    cout << sc_time_stamp() << ": CPU sends write" << endl;

                    uint32_t data = rand();
                    Port_Data.write(data);
                    wait();
                    Port_Data.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
                }
                else
                {
                    cout << sc_time_stamp() << ": CPU sends read" << endl;
                }

                wait(Port_Done.value_changed_event());

                if (f == Cache::FUNC_READ)
                {
                    cout << sc_time_stamp() << ": CPU reads: " << Port_Data.read() << endl;
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

        cache.Port_Func_Mem(sigMemFunc);
        cache.Port_Addr_Mem(sigMemAddr);
        cache.Port_Data(sigMemData);
        cache.Port_Done_Mem(sigMemDone);

        cache.Port_Func_Cpu(sigCpuFunc);
        cache.Port_Addr_Cpu(sigCpuAddr);
        cache.Port_Data(sigCpuData);
        cache.Port_Done_Cpu(sigCpuDone);

        cpu.Port_Func(sigCpuFunc);
        cpu.Port_Addr(sigCpuAddr);
        cpu.Port_Data(sigCpuData);
        cpu.Port_Done(sigCpuDone);

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
