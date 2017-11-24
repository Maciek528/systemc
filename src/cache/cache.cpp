#include "aca2009.h"
#include <systemc.h>
#include <iostream>
#include <list>
#include <fstream>

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

// Initialize logger object
std::ofstream logger("logger.log", std::ios_base::out | std::ios_base::trunc);

SC_MODULE(Cache)
{
public:
  sc_in<bool>       Port_CLK;
  sc_out<RetCode>   Port_CpuDone;
  sc_in<Function>   Port_CpuFunc;
  sc_in<int>        Port_CpuAddr;
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
    logger << "[Cache][execute] " << "start" << endl;
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
      //logger << "Index: " << index << "   Tag: " << tag << endl;

      int linePosition = set[index].findTag(tag);
      int numOfEntries = set[index].numOfEntries;

      Port_Index.write(index);
      Port_Tag.write(tag);
      Port_NumOfEntries.write(numOfEntries);

      if (f == FUNC_WRITE)
      {
        cout << sc_time_stamp() << ": CACHE received write" << endl;
        //logger << sc_time_stamp() << ": CACHE received write" << endl;

        data = Port_CpuData.read().to_int();
        Port_ReadWrite.write(false);
      }
      else
      {
        cout << sc_time_stamp() << ": CACHE received read" << endl;
        //logger << sc_time_stamp() << ": CACHE received read" << endl;

        Port_ReadWrite.write(true);
      }

      if (f == FUNC_READ)
      {
        if (linePosition > -1) {
          set[index].reorderHit(linePosition);
          cout << "READ HIT" << endl;
        //  logger << "READ HIT" << endl;
          stats_readhit(0);
          Port_HitMiss.write(true);
          hitRate++;
        }
        else {
          wait(100); // simulate memory access penalty
          set[index].reorderMiss(numOfEntries, tag, data);
          cout << "READ MISS" << endl;
        //  logger << "READ MISS" << endl;
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
          //logger << "WRITE HIT" << endl;
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
          //logger << "WRITE MISS" << endl;
          stats_writemiss(0);
          Port_HitMiss.write(false);
          missRate++;
        }
        wait();
        Port_CpuDone.write( RET_WRITE_DONE );
      }

      for (int i=0 ; i<NUM_LINES ; i++) {
        cout << "Line : " << i << "   Tag: " << set[index].line[i].tag << endl;
        //logger << "Line : " << i << "   Tag: " << set[index].line[i].tag << endl;
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
    logger << "[CPU][execute] " << "start" << endl;

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

SC_MODULE(ProcessingUnit)
{
public:
  sc_in<bool>       Port_CLK;


  //sc_in<Function>   Port_CpuFunc;
  //sc_in<int>        Port_CpuAddr;
  //sc_out<RetCode>   Port_CpuDone;
  //sc_inout_rv<32>   Port_CpuData;

  sc_signal<int>        sigIndex;
  sc_signal<int>        sigTag;
  sc_signal<int>        sigNumOfEntries;
  sc_signal<bool>       sigReadWrite;
  sc_signal<bool>       sigHitMiss;

  CPU *cpu;
  Cache *cache;

  // // Signals
  // sc_buffer<Function> sigCpuFunc;
  // sc_buffer<RetCode>  sigCpuDone;
  // sc_signal<int>      sigCpuAddr;
  // sc_signal_rv<32>    sigCpuData;

  //sc_out<int>        Port_Index;
  //sc_out<int>        Port_Tag;
  //sc_out<int>        Port_NumOfEntries;
  //sc_out<bool>       Port_ReadWrite;
  //sc_out<bool>       Port_HitMiss;

  // class Line {
  // public:
  //   int   tag;
  //   int   data;
  //
  //   Line() {
  //     tag = -1;
  //     data = 0;
  //   }
  // };
  //
  // class Set {
  // public:
  //   int numOfEntries;
  //   Line line[NUM_LINES];
  //   Set (){
  //     numOfEntries = 0;
  //   };
  //
  //   int findTag (int tag) {
  //     for(int i=0 ; i<NUM_LINES ; i++){
  //       if (line[i].tag == tag){
  //         return i;
  //       }
  //     }
  //     return -1;
  //   }
  //
  //   /* Shift all lines right up to the added line position.
  //   If the set is full, the entry from its last position is deleted. */
  //   void shiftLinesMiss(int numOfEntries) {
  //     for(int i = numOfEntries; i > 1 ; i--) {
  //       line[i-1] = line[i-2];
  //     }
  //   }
  //   void shiftLinesHit(int linePosition) {
  //     for(int i = 0; i < linePosition-1 ; i++) {
  //       line[i+1] = line[i];
  //     }
  //   }
  //
  //   /* Reorder functions move around lines in a set, depending on whether it's a hit or miss.
  //    The first position in a set is always occupied by the MRU line.*/
  //   void reorderMiss(int numOfEntries, int tag, int data) {
  //     if (numOfEntries == 1){
  //       line[1] = line[0];
  //     } else {
  //       shiftLinesMiss(numOfEntries);
  //     }
  //     line[MRU_POSITION].tag = tag;
  //     line[MRU_POSITION].data = data;
  //     if (this->numOfEntries < NUM_LINES)
  //     this->numOfEntries++;
  //   }
  //
  //   void reorderHit(int linePosition) {
  //     if (linePosition == 0) {
  //       ;
  //     } else if (linePosition == 1) {
  //       swap(line[MRU_POSITION], line [linePosition]);
  //     } else {
  //       Line temp = line[linePosition];
  //       shiftLinesHit(linePosition);
  //       line[MRU_POSITION] = temp;
  //     }
  //   }
  // };

  SC_CTOR(ProcessingUnit)
  {
    // Signals
      sc_buffer<Function> sigCpuFunc;
      sc_buffer<RetCode>  sigCpuDone;
      sc_signal<int>      sigCpuAddr;
      sc_signal_rv<32>    sigCpuData;

    // Create and patch CPU
      cpu = new CPU("cpu");

      cpu->Port_CacheFunc(sigCpuFunc);
      cpu->Port_CacheAddr(sigCpuAddr);
      cpu->Port_CacheData(sigCpuData);
      cpu->Port_CacheDone(sigCpuDone);
      cpu->Port_CLK(Port_CLK);
    logger << "cpu done" << endl;
    //

    // Create and patch Cache
      cache = new Cache("cache");

      cache->Port_CpuFunc(sigCpuFunc);
      cache->Port_CpuAddr(sigCpuAddr);
      cache->Port_CpuData(sigCpuData);
      cache->Port_CpuDone(sigCpuDone);
      cache->Port_CLK(Port_CLK);
      // signals for output trace
      cache->Port_Index(sigIndex);
      cache->Port_Tag(sigTag);
      cache->Port_NumOfEntries(sigNumOfEntries);
      cache->Port_ReadWrite(sigReadWrite);
      cache->Port_HitMiss(sigHitMiss);
    logger << "cache done" << endl;


    SC_THREAD(execute);
    logger << "thread started" << endl;
    sensitive << Port_CLK.pos();
    dont_initialize();
  }

private:

  // CPU cpu("cpu");
  // Cache cache("cache");
  //
  // // Signals
  // sc_buffer<Function> sigCpuFunc;
  // sc_buffer<RetCode>  sigCpuDone;
  // sc_signal<int>      sigCpuAddr;
  // sc_signal_rv<32>    sigCpuData;
  //
  // sc_signal<int>      sigIndex;
  // sc_signal<int>      sigTag;
  // sc_signal<int>      sigNumOfEntries;
  // sc_signal<bool>     sigReadWrite;
  // sc_signal<bool>     sigHitMiss;
  //
  // // The clock that will drive the CPU and cache
  // sc_clock clk;
  //
  // // Connecting module ports with signals
  // cache.Port_CpuFunc(sigCpuFunc);
  // cache.Port_CpuAddr(sigCpuAddr);
  // cache.Port_CpuData(sigCpuData);
  // cache.Port_CpuDone(sigCpuDone);
  //
  //  cpu.Port_CacheFunc(sigCpuFunc);
  //  cpu.Port_CacheAddr(sigCpuAddr);
  //  cpu.Port_CacheData(sigCpuData);
  //  cpu.Port_CacheDone(sigCpuDone);
  //
  //
  //  cpu.Port_CLK(clk);
  //  cache.Port_CLK(clk);
  //
  //
  //  // signals for output trace
  //  cache.Port_Index(sigIndex);
  //  cache.Port_Tag(sigTag);
  //  cache.Port_NumOfEntries(sigNumOfEntries);
  //  cache.Port_ReadWrite(sigReadWrite);
  //  cache.Port_HitMiss(sigHitMiss);



  // int getIndex (int address) {
  //   return (address & 0x0000FE0) >> 5;
  // }
  //
  // int getTag (int address) {
  //   return (address & 0xFFFFF000) >> 12;
  // }

  void execute()
  {
    logger << "[PU][execute] " << "start" << endl;


  }
};


int sc_main(int argc, char* argv[])
{
  try
  {
    logger << "test" << endl;

    // Get the tracefile argument and create Tracefile object
    // This function sets tracefile_ptr and num_cpus
    init_tracefile(&argc, &argv);
    logger << "[main] " << "tracefile inited" << endl;

    // Initialize statistics counters
    stats_init();
    logger << "[main] " << "stats inited" << endl;

    // Instantiate Modules
    //CPU    cpu("cpu");
    //Cache  cache("cache");

    // The clock that will drive the CPU and Cache
    sc_clock clk;

    logger << "[main] " << "clock created" << endl;

    ProcessingUnit processingUnit("pu");

    logger << "main" << "processingUnit created" << endl;

    // Signals
    // sc_buffer<Function>   sigCpuFunc;
    // sc_buffer<RetCode>    sigCpuDone;
    // sc_signal<int>        sigCpuAddr;
    // sc_signal_rv<32>      sigCpuData;
    //
    // sc_signal<int>        sigIndex;
    // sc_signal<int>        sigTag;
    // sc_signal<int>        sigNumOfEntries;
    // sc_signal<bool>       sigReadWrite;
    // sc_signal<bool>       sigHitMiss;


    // Connecting module ports with signals
    // cache.Port_CpuFunc(sigCpuFunc);
    // cache.Port_CpuAddr(sigCpuAddr);
    // cache.Port_CpuData(sigCpuData);
    // cache.Port_CpuDone(sigCpuDone);
    //
    //  cpu.Port_CacheFunc(sigCpuFunc);
    //  cpu.Port_CacheAddr(sigCpuAddr);
    //  cpu.Port_CacheData(sigCpuData);
    //  cpu.Port_CacheDone(sigCpuDone);

    //processingUnit.Port_CpuFunc(sigCpuFunc);
    //processingUnit.Port_CpuAddr(sigCpuAddr);
    //processingUnit.Port_PUData(sigCpuData);
    //processingUnit.Port_PUDone(sigCpuDone);



    // cpu.Port_CLK(clk);
    // cache.Port_CLK(clk);



    processingUnit.Port_CLK(clk);

    logger << "[main] " << "Processing unit patched with clock" << endl;




    // Open VCD file
    // sc_trace_file *wf = sc_create_vcd_trace_file("cache_results");
    // sc_trace(wf, clk, "Clock");
    // sc_trace(wf, sigIndex, "Index");
    // sc_trace(wf, sigTag, "Tag");
    // sc_trace(wf, sigNumOfEntries, "NumOfEntries");
    // sc_trace(wf, sigReadWrite, "Read/Write");
    // sc_trace(wf, sigHitMiss, "Hit/Miss");

    hitRate = 0;
    missRate = 0;

    logger << "[main] " << "hitmissrate defined" << endl;


    cout << "Running (press CTRL+C to interrupt)... " << endl;

    // Start Simulation
    sc_start();

    logger << "[main] " << "simulation started" << endl;


    // Print statistics after simulation finished
    stats_print();
    cout << endl;
    cout << "Avarage mem access time:" << (hitRate + missRate * 100) / (hitRate + missRate) << endl;
    cout << endl;
    //sc_close_vcd_trace_file(wf);
  }

  catch (exception& e)
  {
    cerr << e.what() << endl;
  }

  logger.close();
  return 0;
}
