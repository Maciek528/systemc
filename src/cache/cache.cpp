#include "aca2009.h"
#include <systemc.h>
#include <iostream>
#include <list>
#include <vector>


using namespace std;

static const int NUM_SETS = 128;
static const int NUM_LINES = 8;
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

double hitRate, missRate;

SC_MODULE(Cache)
{
public:
  sc_in<bool>       Port_CLK;
  sc_in<Function>   Port_CpuFunc;
  sc_in<int>        Port_CpuAddr;
  sc_out<RetCode>   Port_CpuDone;
  sc_inout_rv<32>   Port_CpuData;

  sc_out<int>        Port_Index;
  sc_out<int>        Port_Tag;
  sc_out<int>        Port_NumOfEntries;
  sc_out<bool>       Port_ReadWrite;
  sc_out<bool>       Port_HitMiss;

  class Line {
  public:
    int   tag;
    int   data;

    Line() {
      tag = -1;
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

    /* Shift all lines right up to the added line position.
    If the set is full, the entry from its last position is deleted. */
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

    /* Reorder functions move around lines in a set, depending on whether it's a hit or miss.
     The first position in a set is always occupied by the MRU line.*/
    void reorderMiss(int numOfEntries, int tag, int data) {
      if (numOfEntries == 1){
        line[1] = line[0];
      } else {
        shiftLinesMiss(numOfEntries);
      }
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
    // Build an array of sets, where each set is an array of lines.
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

      Port_Index.write(index);
      Port_Tag.write(tag);
      Port_NumOfEntries.write(numOfEntries);

      if (f == FUNC_WRITE)
      {
        cout << sc_time_stamp() << ": CACHE received write" << endl;
        data = Port_CpuData.read().to_int();
        Port_ReadWrite.write(false);
      }
      else
      {
        cout << sc_time_stamp() << ": CACHE received read" << endl;
        Port_ReadWrite.write(true);
      }

      if (f == FUNC_READ)
      {
        if (linePosition > -1) {
          set[index].reorderHit(linePosition);
          cout << "READ HIT" << endl;
          stats_readhit(0);
          Port_HitMiss.write(true);
          hitRate++;
        }
        else {
          wait(100); // simulate memory access penalty
          set[index].reorderMiss(numOfEntries, tag, data);
          cout << "READ MISS" << endl;
          stats_readmiss(0);
          Port_HitMiss.write(false);
          missRate++;
        }

        Port_CpuDone.write( RET_READ_DONE );

      }
      else //writing
      {
        if (linePosition > -1) {
          set[index].reorderHit(linePosition);
          cout << "WRITE HIT" << endl;
          stats_writehit(0);
          Port_HitMiss.write(true);
          hitRate++;
        }
        else {
          if (numOfEntries == NUM_LINES) {
            wait(100); // set is full => writeback
          }
          set[index].reorderMiss(numOfEntries, tag, data);
          cout << "WRITE MISS" << endl;
          stats_writemiss(0);
          Port_HitMiss.write(false);
          missRate++;
        }
        wait();
        Port_CpuDone.write( RET_WRITE_DONE );
      }

      // for (int i=0 ; i<NUM_LINES ; i++) {
      //   cout << "Line : " << i << "   Tag: " << set[index].line[i].tag << endl;
      // }

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

  int _pid;

  void setPid (int pid) {
    _pid = pid;
  }

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
      if(!tracefile_ptr->next(_pid, tr_data))
      {
        cerr << "Error reading trace for CPU" << endl;
        break;
      }

      cout << "CPU " << _pid << " executing" << endl;

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
    cout << "Total runtime: " << sc_time_stamp() << endl;
  }
};


int sc_main(int argc, char* argv[])
{
  try
  {
    // Get the tracefile argument and create Tracefile object
    // This function sets tracefile_ptr and num_cpus
    init_tracefile(&argc, &argv);
    cout << "num of cups " << num_cpus << endl;
    vector<CPU> cpus(num_cpus);

    // Initialize statistics counters
    stats_init();

    // Instantiate Modules
    for (int i=0 ; i<num_cpus ; i++){
      CPU cpu("cpu");
      cpu.setPid(i);
      cpus[i] = cpu;
    }
    // CPU cpu("cpu");
    Cache  cache("cache");

    // Signals
    sc_buffer<Function>   sigCpuFunc;
    sc_buffer<RetCode>    sigCpuDone;
    sc_signal<int>        sigCpuAddr;
    sc_signal_rv<32>      sigCpuData;

    sc_signal<int>        sigIndex;
    sc_signal<int>        sigTag;
    sc_signal<int>        sigNumOfEntries;
    sc_signal<bool>       sigReadWrite;
    sc_signal<bool>       sigHitMiss;

    // The clock that will drive the CPU and Cache
    sc_clock clk;

    // Connecting module ports with signals
    cache.Port_CpuFunc(sigCpuFunc);
    cache.Port_CpuAddr(sigCpuAddr);
    cache.Port_CpuData(sigCpuData);
    cache.Port_CpuDone(sigCpuDone);

    cpus[0].Port_CacheFunc(sigCpuFunc);
    cpus[0].Port_CacheAddr(sigCpuAddr);
    cpus[0].Port_CacheData(sigCpuData);
    cpus[0].Port_CacheDone(sigCpuDone);

    cpus[0].Port_CLK(clk);
    cache.Port_CLK(clk);

    // signals for output trace
    cache.Port_Index(sigIndex);
    cache.Port_Tag(sigTag);
    cache.Port_NumOfEntries(sigNumOfEntries);
    cache.Port_ReadWrite(sigReadWrite);
    cache.Port_HitMiss(sigHitMiss);

    // Open VCD file
    sc_trace_file *wf = sc_create_vcd_trace_file("cache_results");
    sc_trace(wf, clk, "Clock");
    sc_trace(wf, sigIndex, "Index");
    sc_trace(wf, sigTag, "Tag");
    sc_trace(wf, sigNumOfEntries, "NumOfEntries");
    sc_trace(wf, sigReadWrite, "Read/Write");
    sc_trace(wf, sigHitMiss, "Hit/Miss");

    hitRate = 0;
    missRate = 0;

    cout << "Running (press CTRL+C to interrupt)... " << endl;

    // Start Simulation
    sc_start();

    // Print statistics after simulation finished
    stats_print();
    cout << endl;
    cout << "Avarage mem access time:" << (hitRate + missRate * 100) / (hitRate + missRate) << endl;
    cout << endl;
    sc_close_vcd_trace_file(wf);
  }

  catch (exception& e)
  {
    cerr << e.what() << endl;
  }

  return 0;
}
[0]
