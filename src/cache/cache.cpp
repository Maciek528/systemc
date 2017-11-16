#include "aca2009.h"
#include <systemc.h>
#include <iostream>
#include <list>

using namespace std;

static const int NUM_SETS = 128;
static const int NUM_LINES = 8;
static const int LRU_POSITION = NUM_LINES - 1;
static const int MRU_POSITION = 0;

enum Function
{
  FUNC_READ,
  FUNC_WRITE
};

enum RetCode
{
  RET_READ_DONE,
  RET_WRITE_DONE
};


SC_MODULE(Cache)
{
public:
  sc_in<bool>       Port_CLK;
  sc_in<Function>   Port_CpuFunc;
  sc_in<int>        Port_CpuAddr;
  sc_out<RetCode>   Port_CpuDone;
  sc_inout_rv<32>   Port_CpuData;

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
    int numOfEntries;
    Line line[NUM_LINES];
    Set (){
      numOfEntries = 0;
    };

    int findTag (int tag) {
      for(int i=0 ; i<NUM_LINES ; i++){
        if (line[i].tag == tag){
          return i;
        }
      }
      return -1;
    }

    // Shift all lines right up to the added line position. Remove from the LRU position.
    void shiftLinesMiss(int numOfEntries) {
      for(int i = numOfEntries; i > 1 ; i--) {
        line[i-1] = line[i-2];
      }
    }
    void shiftLinesHit(int linePosition) {
      for(int i = 0; i < linePosition-1 ; i++) {
        line[i+1] = line[i];
      }
    }

    void reorderMiss(int numOfEntries, int tag, int data) {
      if (numOfEntries == 1){
        line[1] = line[0];
      } else {
        shiftLinesMiss(numOfEntries);
      }
      line[MRU_POSITION].isValid = true;
      line[MRU_POSITION].tag = tag;
      line[MRU_POSITION].data = data;
      if (this->numOfEntries < NUM_LINES)
        this->numOfEntries++;
    }

    void reorderHit(int linePosition) {
      if (linePosition == 0) {
        ;
      } else if (linePosition == 1) {
        swap(line[MRU_POSITION], line [linePosition]);
      } else {
        Line temp = line[linePosition];
        shiftLinesHit(linePosition);
        line[MRU_POSITION] = temp;
      }
    }
  };

  SC_CTOR(Cache)
  {
    SC_THREAD(execute);
    sensitive << Port_CLK.pos();
    dont_initialize();
  }

private:

  int getIndex (int address) {
    return (address & 0x0000FE0) >> 5;
  }

  int getTag (int address) {
    return (address & 0xFFFFF000) >> 12;
  }

  void execute()
  {
    Set set[NUM_SETS];
    for (int i=0 ; i<NUM_SETS ; i++){
      for (int j=0 ; j<NUM_LINES ; j++){
        Line newLine;
        set[i].line[j] = newLine;
      }
    }

    while (true)
    {
      wait(Port_CpuFunc.value_changed_event());	// this is fine since we use sc_buffer

      Function f = Port_CpuFunc.read();
      int addr   = Port_CpuAddr.read();
      int index  = getIndex(addr);
      int tag    = getTag(addr);
      int data   = 0;

      cout << "Index: " << index << "   Tag: " << tag << endl;

      int linePosition = set[index].findTag(tag);
      int numOfEntries = set[index].numOfEntries;

      // cout << "linePosition: " << linePosition << endl;
      // cout << "numOfEntries: " << numOfEntries << endl;

      if (f == FUNC_WRITE)
      {
        cout << sc_time_stamp() << ": CACHE received write" << endl;
        data = Port_CpuData.read().to_int();

      }
      else
      {
        cout << sc_time_stamp() << ": CACHE received read" << endl;
      }

      if (f == FUNC_READ)
      {
        if (linePosition > -1) {
          cout << "READ HIT" << endl;
          stats_readhit(0);

          set[index].reorderHit(linePosition);

        }
        else {
          cout << "READ MISS" << endl;
          stats_readmiss(0);
          wait(100); // simulate memory access penalty

          set[index].reorderMiss(numOfEntries, tag, data);
        }

        Port_CpuDone.write( RET_READ_DONE );

      }
      else //writing
      {
        if (linePosition > -1) {
          cout << "WRITE HIT" << endl;
          stats_writehit(0);

          set[index].reorderHit(linePosition);
        }
        else {
          cout << "WRITE MISS" << endl;
          stats_writemiss(0);

          if (set[index].line[LRU_POSITION].isValid) {
            wait(100); // writeback
          }

          set[index].reorderMiss(numOfEntries, tag, data);
        }
        wait();
        Port_CpuDone.write( RET_WRITE_DONE );
      }

      for (int i=0 ; i<NUM_LINES ; i++) {
          cout << "Line : " << i << "   Tag: " << set[index].line[i].tag << "   Valid: " << set[index].line[MRU_POSITION].isValid << endl;
      }

    }
  }
};


SC_MODULE(CPU)
{

public:
  sc_in<bool>                 Port_CLK;
  sc_in<RetCode>              Port_CacheDone;
  sc_out<Function>            Port_CacheFunc;
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
    Function  f;

    // Loop until end of tracefile
    while(!tracefile_ptr->eof())
    {
      // Get the next action for the processor in the trace
      if(!tracefile_ptr->next(0, tr_data))
      {
        cerr << "Error reading trace for CPU" << endl;
        break;
      }

      switch(tr_data.type)
      {
        case TraceFile::ENTRY_TYPE_READ:
        f = FUNC_READ;
        break;

        case TraceFile::ENTRY_TYPE_WRITE:
        f = FUNC_WRITE;
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

        if (f == FUNC_WRITE)
        {
          cout << sc_time_stamp() << ": CPU sends write" << endl;

          uint32_t data = rand();
          Port_CacheData.write(data);
          wait();
          // Port_CacheData.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
        }
        else
        {
          cout << sc_time_stamp() << ": CPU sends read" << endl;
        }

        // cout << "waiting" << endl;
        wait(Port_CacheDone.value_changed_event());
        // cout << "value changed" << endl;

        if (f == FUNC_READ)
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
      cout << endl;
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
    CPU    cpu("cpu");
    Cache  cache("cache");

    // Signals
    sc_buffer<Function>   sigCpuFunc;
    sc_buffer<RetCode>    sigCpuDone;
    sc_signal<int>        sigCpuAddr;
    sc_signal_rv<32>      sigCpuData;

    // The clock that will drive the CPU and Cache
    sc_clock clk;

    // Connecting module ports with signals
    cache.Port_CpuFunc(sigCpuFunc);
    cache.Port_CpuAddr(sigCpuAddr);
    cache.Port_CpuData(sigCpuData);
    cache.Port_CpuDone(sigCpuDone);

    cpu.Port_CacheFunc(sigCpuFunc);
    cpu.Port_CacheAddr(sigCpuAddr);
    cpu.Port_CacheData(sigCpuData);
    cpu.Port_CacheDone(sigCpuDone);

    cpu.Port_CLK(clk);
    cache.Port_CLK(clk);

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
