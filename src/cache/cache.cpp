#include "aca2009.h"
#include <systemc.h>
#include <iostream>
#include <list>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>

using namespace std;

static const int NUM_SETS = 128;
static const int NUM_LINES = 8;
static const int MRU_POSITION = 0;

sc_mutex traceFileMtx;
sc_mutex doneProcessesMtx;

int numProcessesDone = 0;
int gNumProcesses;

// Functions

template<typename S>
string to_string(S var)
{
  ostringstream temp;
  temp << var;
  return temp.str();
};

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

//SC_MODULE(Cache)
class Cache : public sc_module
{
public:
  // Clock
    sc_in<bool>       Port_CLK;

  // Ports to CPU
    sc_out<RetCode>   Port_CpuDone;
    sc_in<Function>   Port_CpuFunc;
    sc_in<int>        Port_CpuAddr;
    sc_inout_rv<32>   Port_CpuData;

  // Ports (tracefile)
    sc_out<int>        Port_Index;
    sc_out<int>        Port_Tag;
    sc_out<int>        Port_NumOfEntries;
    sc_out<bool>       Port_ReadWrite;
    sc_out<bool>       Port_HitMiss;

    // has to be added when no standard constructor SC_CTOR is used
    SC_HAS_PROCESS(Cache);

  // Inner Classes
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
        }  else if (linePosition == 1) {
          swap(line[MRU_POSITION], line [linePosition]);
        } else {
          Line temp = line[linePosition];
          shiftLinesHit(linePosition);
          line[MRU_POSITION] = temp;
        }
      }
  };

  // Custom constructor
  Cache(sc_module_name nm, int pid): sc_module(nm), pid_(pid) {
    SC_THREAD(execute);
    sensitive << Port_CLK.pos();
    // Perhaps dont_initialize() can be executed
    //dont_initialize();
  }

private:
  int pid_;

  int getIndex (int address) {
    return (address & 0x0000FE0) >> 5;
  }

  int getTag (int address) {
    return (address & 0xFFFFF000) >> 12;
  }

  void execute()
  {
    //logger << "[Cache" << pid_ << "][execute] " << "start" << endl;
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

      //cout << "Index: " << index << "   Tag: " << tag << endl;
      //logger << "Index: " << index << "   Tag: " << tag << endl;

      int linePosition = set[index].findTag(tag);
      int numOfEntries = set[index].numOfEntries;

      Port_Index.write(index);
      Port_Tag.write(tag);
      Port_NumOfEntries.write(numOfEntries);

      if (f == FUNC_WRITE)
      {
        //cout << sc_time_stamp() << ": CACHE received write" << endl;
        //logger << sc_time_stamp() << ": CACHE received write" << endl;

        data = Port_CpuData.read().to_int();
        Port_ReadWrite.write(false);
      }
      else
      {
        //cout << sc_time_stamp() << ": CACHE received read" << endl;
        //logger << sc_time_stamp() << ": CACHE received read" << endl;

        Port_ReadWrite.write(true);
      }

      if (f == FUNC_READ)
      {
        if (linePosition > -1) {
          set[index].reorderHit(linePosition);
          //cout << "READ HIT" << endl;
        //  logger << "READ HIT" << endl;
          stats_readhit(pid_);
          Port_HitMiss.write(true);
          hitRate++;
        }
        else {
          wait(100); // simulate memory access penalty
          set[index].reorderMiss(numOfEntries, tag, data);
          //cout << "READ MISS" << endl;
        //  logger << "READ MISS" << endl;
          stats_readmiss(pid_);
          Port_HitMiss.write(false);
          missRate++;
        }

        Port_CpuDone.write( RET_READ_DONE );

      }
      else //writing
      {
        if (linePosition > -1) {
          set[index].reorderHit(linePosition);
          //cout << "WRITE HIT" << endl;
          //logger << "WRITE HIT" << endl;
          stats_writehit(pid_);
          Port_HitMiss.write(true);
          hitRate++;
        }
        else {
          if (numOfEntries == NUM_LINES) {
            wait(100); // set is full => writeback
          }
          set[index].reorderMiss(numOfEntries, tag, data);
          //cout << "WRITE MISS" << endl;
          //logger << "WRITE MISS" << endl;
          stats_writemiss(pid_);
          Port_HitMiss.write(false);
          missRate++;
        }
        wait();
        Port_CpuDone.write( RET_WRITE_DONE );
      }

      for (int i=0 ; i<NUM_LINES ; i++) {
        //cout << "Line : " << i << "   Tag: " << set[index].line[i].tag << endl;
        //logger << "Line : " << i << "   Tag: " << set[index].line[i].tag << endl;
      }

    }
  }
};


//SC_MODULE(CPU)
class CPU : public sc_module
{

public:
  // Clock
    sc_in<bool>                 Port_CLK;

  // Connection to Cache
    sc_in<RetCode>              Port_CacheDone;
    sc_out<Function>            Port_CacheFunc;
    sc_out<int>                 Port_CacheAddr;
    sc_inout_rv<32>             Port_CacheData;

    // has to be added when no standard constructor SC_CTOR is used
    SC_HAS_PROCESS(CPU);

    // Custom constructor
    CPU(sc_module_name name, int pid) : sc_module(name), pid_(pid)
    {
      iNumber_ = 0;
      SC_THREAD(execute);
      sensitive << Port_CLK.pos();
    }


private:
  int pid_;
  int iNumber_;
  bool isDone_;

  void execute()
  {
    //logger << "[CPU" << pid_ << "][execute] " << "start" << endl;

    bool isDone_ = false;

     TraceFile::Entry    tr_data;
     Function  f;
     bool gotNext;

     // Loop until end of tracefile
     traceFileMtx.lock();
     bool endOfFile = tracefile_ptr->eof();
     traceFileMtx.unlock();
     //logger << "[CPU" << pid_ << "][execute] " << "got endOfFile" << endl;


     while(!endOfFile)
     {
       // Get the next action for the processor in the trace
       traceFileMtx.lock();
       gotNext   = tracefile_ptr->next(pid_, tr_data);
       traceFileMtx.unlock();
       //logger << "[CPU" << pid_ << "][execute] " << "read instruction #" << iNumber_ << endl;
       iNumber_++;
       if(!gotNext)
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
         //logger << "[CPU" << pid_ << "] Error, got invalid data from Trace" << endl;
         exit(0);
       }

      if(tr_data.type != TraceFile::ENTRY_TYPE_NOP)
      {
        Port_CacheAddr.write(tr_data.addr);
        Port_CacheFunc.write(f);

        if (f == FUNC_WRITE)
        {
          cout << sc_time_stamp() << ": [CPU" << pid_ << "] sends write" << endl;

          uint32_t data = rand();
          Port_CacheData.write(data);
          wait();

        }
        else
        {
          cout << sc_time_stamp() << ": [CPU" << pid_ << "] sends read" << endl;
        }

        // cout << "waiting" << endl;
        wait(Port_CacheDone.value_changed_event());
        // cout << "value changed" << endl;

        if (f == FUNC_READ)
        {
          cout << sc_time_stamp() << ": [CPU" << pid_ << "] reads: " << Port_CacheData.read() << endl;
        }
      }
      else
      {
        cout << sc_time_stamp() << ": [CPU" << pid_ << "] executes NOP" << endl;
      }

      // chceck if end of file
      endOfFile = tracefile_ptr->eof();

      // Advance one cycle in simulated time
      wait();
      cout << endl;
    }

    if( !isDone_ )
    {
      doneProcessesMtx.lock();
      numProcessesDone++;
      doneProcessesMtx.unlock();
      isDone_ = true;
    }
    if(numProcessesDone == gNumProcesses)
    {
      sc_stop();
      logger << "Simulation stopped" << endl;
      cout << "Total runtime: " << sc_time_stamp() << endl;
    }

  }
};

class ProcessingUnit : public sc_module
{
public:
  // Clock
  sc_in<bool>       Port_CLK;

  // Signals
  sc_buffer<Function> sigCpuFunc;
  sc_buffer<RetCode>  sigCpuDone;
  sc_signal<int>      sigCpuAddr;
  sc_signal_rv<32>    sigCpuData;

  // has to be added when no standard constructor SC_CTOR is used
  SC_HAS_PROCESS(ProcessingUnit);

  // Custom constructor
  ProcessingUnit(sc_module_name name, int pid) : sc_module(name), pid_(pid)
  {
    // Create and patch CPU
      cpu = new CPU("cpu", pid_);

      cpu->Port_CacheFunc(sigCpuFunc);
      cpu->Port_CacheAddr(sigCpuAddr);
      cpu->Port_CacheData(sigCpuData);
      cpu->Port_CacheDone(sigCpuDone);
      cpu->Port_CLK(Port_CLK);
    logger << "[PU" << pid_ << "] cpu created" << endl;

    // Create and patch Cache
      cache = new Cache("cache", pid_);

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
    logger << "[PU" << pid_ << "] cache created" << endl;


    SC_THREAD(execute);
    logger << "[PU" << pid_ << "] thread registered" << endl;
    sensitive << Port_CLK.pos();
    // perhaps dont_initialize() can be executed
    //dont_initialize();
  }

private:
  int pid_;

  CPU *cpu;
  Cache *cache;

  sc_signal<int>        sigIndex;
  sc_signal<int>        sigTag;
  sc_signal<int>        sigNumOfEntries;
  sc_signal<bool>       sigReadWrite;
  sc_signal<bool>       sigHitMiss;

  void execute()
  {
    logger << "[PU" << pid_ << "] [execute] " << "start" << endl;

  }
};

int sc_main(int argc, char* argv[])
{

  // Variables
  int num_procs = -1;


  try
  {
    logger << "[main] start" << endl;

    // Get the tracefile argument and create Tracefile object
    // This function sets tracefile_ptr and num_cpus
    init_tracefile(&argc, &argv);
    logger << "[main] " << "tracefile inited" << endl;

    // Initialize statistics counters
    stats_init();
    logger << "[main] " << "stats inited" << endl;

    num_procs = tracefile_ptr->get_proc_count();
    gNumProcesses = num_procs;

    // The clock that will drive the PU's, CPU and Cache
    sc_clock clk;


    logger << "[main] " << "clock created" << endl;
    logger << "[main] " << "num_proc: " << tracefile_ptr->get_proc_count() << endl;

    // Create a vector of pointers to processing units
    std::vector<ProcessingUnit*> processingUnits;

    for( int i = 0; i < num_procs; i++ )
    {
      // Create processing unit with given PID
      ProcessingUnit* processingUnit = new ProcessingUnit("pu", i);
      processingUnit->Port_CLK(clk);
      // Push into vector
      processingUnits.push_back(processingUnit);
    }

    logger << "[main] "  << "processingUnits created" << endl;
    logger << "[main] "  << "processingUnits.size(): " << processingUnits.size() << endl;

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
