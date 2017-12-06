#include "aca2009.h"
#include <systemc.h>
#include <iostream>
#include <list>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>

//#define SC_DEFAULT_WRITER_POLICY SC_MANY_WRITERS



using namespace std;

static const int NUM_SETS = 128;
static const int NUM_LINES = 8;
static const int MRU_POSITION = 0;

sc_mutex traceFileMtx;
sc_mutex doneProcessesMtx;

int numProcessesDone = 0;
int gNumProcesses;


static const int MEM_SIZE = 512;

static const int Cache_Size = 32768;    // Total L1 Cache Size is 32 KByte (only for data)
static const int Cache_Line = 32;       // Size of one cache Line is 32 byte
static const int Cache_Line_Count = Cache_Size / Cache_Line ; // number of total lines is 1024
static const int n_Set_Association = 8;  // The cache is 8 way set associative
static const int Set_Count = Cache_Line_Count / n_Set_Association ; // Number of sets in this cache is 128

static const int M_Memory_Size = Cache_Size * 16; // This variable is not defined. It is the total size of the main memory
static const int M_Memory_Block_Count = Set_Count; // Block number is always the same as the set number
static const int M_Memory_Block_Size = M_Memory_Size / M_Memory_Block_Count; //The size of one Block in the Memory
static const int Line_Count_per_Block = M_Memory_Block_Size / Cache_Line ; // The number of Lines in each block


// Function type
enum Function
{
    F_INVALID,
    F_READ,
    F_WRITE,
};

enum RetCode
{
    RET_READ_DONE,
    RET_WRITE_DONE
};

double hitRate, missRate;

// Initialize logger object
std::ofstream logger("logger.log", std::ios_base::out | std::ios_base::trunc);

// Simple Bus interface
class Bus_if : public virtual sc_interface
{
public:
    virtual bool read(int writer, int addr, bool Exclusive) = 0;
    virtual bool write(int writer, int addr, int data) = 0;
};

/* Bus class, provides a way to share one memory in multiple CPU + Caches. */
class Bus : public Bus_if, public sc_module {
public:

    /* Ports and Signals. */
    sc_in<bool>         Port_CLK;
    sc_out<Function>    Port_BusValid;
    sc_signal_rv<32>    Port_BusAddr;
    sc_out<int>         Port_BusWriter;

    /* Bus mutex. */
    sc_mutex busMtx;

    /* Variables. */
    long waits;
    long reads;
    long writes;

    // has to be added when no standard constructor SC_CTOR is used
    SC_HAS_PROCESS(Bus);

public:
    /* Constructor. */
    Bus(sc_module_name name) : sc_module(name)
    {
        /* Handle Port_CLK to simulate delay */
        sensitive << Port_CLK.pos();

        // Initialize some bus properties
        Port_BusAddr.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");

        /* Update variables. */
        waits = 0;
        reads = 0;
        writes = 0;
    }


    /* Perform a read access to memory addr for CPU #writer. */
    virtual bool read(int writer, int addr, bool Exclusive){

        /* Try to get exclusive lock on bus. */
        logger<<endl<< "--> CORE "<< writer <<" try to Lock BUS for READ  -->"<<endl;
        while(busMtx.trylock() == -1){
            /* Wait when bus is in contention. */
            waits++;
            wait();
        }

        /* Update number of bus accesses. */
        reads++;
        logger << "--> CORE "<< writer <<" LOCK for READ"<<endl;

        /* Set lines. */
        Port_BusAddr.write(addr);
        Port_BusWriter.write(writer);
        if(Exclusive)
            Port_BusValid.write(F_INVALID);
        else
            Port_BusValid.write(F_READ);

        /* Wait for everyone to recieve. */
        wait(100);   //Simulate Memory Latency

        /* Reset. */
        Port_BusAddr.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
        busMtx.unlock();
        logger << "--> CORE "<< writer << " UNLOCK for READ"<<endl;
        return(true);
    };

    /* Write action to memory, need to know the writer, address and data. */
    virtual bool write(int writer, int addr, int data){

        /* Try to get exclusive lock on the bus. */
        logger<<endl<< "--> CORE "<< writer <<" try to Lock BUS for WRITE  -->"<<endl;
        while(busMtx.trylock() == -1){
            waits++;
            wait();
        }

        /* Update number of accesses. */
        writes++;
        logger << "--> CORE "<< writer <<"L OCK for WRITE"<<endl;

        /* Set. */
        Port_BusAddr.write(addr);
        Port_BusWriter.write(writer);
        Port_BusValid.write(F_WRITE);

        /* Wait for everyone to recieve. */
        wait(100);  //Simulate Memory Latency

        /* Reset. */
        Port_BusAddr.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
        busMtx.unlock();
        logger << "--> CORE "<< writer << " UNLOCK for WRITE"<<endl;
        return(true);
    }

    /* Bus output. */
    void output(){
        /* Write output as specified in the assignment. */
        double avg = (double)waits / double(reads + writes);
        printf("\n 2. Main memory access rates\n");
        printf("    Bus had %ld reads and %ld writes.\n", reads, writes);
        printf("    A total of %ld accesses.\n", reads + writes);
        printf("\n 3. Average time for bus acquisition\n");
        printf("    There were %ld waits for the bus.\n", waits);
        printf("    Average waiting time per access: %f cycles.\n", avg);
    }
};

//SC_MODULE(Cache)
class Cache : public sc_module
{
public:


    // Clock
    sc_in<bool>       Port_CLK;

    // Ports to CPU
    sc_in<Function>   Port_CpuFunc;
    sc_in<int>        Port_CpuAddr;
    sc_out<RetCode>   Port_CpuDone;
    sc_inout_rv<32>   Port_CpuData;

    // Bus snooping ports
    sc_in_rv<32>        Port_BusAddr;
    sc_in<int>       Port_BusWriter;
    sc_in<Function>     Port_BusValid;

    // Bus requests ports
    sc_port<Bus_if, 0>     Port_Bus;

    // Ports (tracefile)
    sc_out<int>        Port_Index;
    sc_out<int>        Port_Tag;
    sc_out<bool>       Port_ReadWrite;
    sc_out<bool>       Port_HitMiss;



    // has to be added when no standard constructor SC_CTOR is used
    SC_HAS_PROCESS(Cache);

    typedef struct {
        int Set_Number;  // index number
        int Tag[n_Set_Association];
        int Data[n_Set_Association];
        bool valid[n_Set_Association];
        bool dirty[n_Set_Association];
        double TimeStamp[n_Set_Association];
    } Cache_Set;


    class Cache_Sets{
    public:
        Cache_Set Set[Set_Count];

        Cache_Sets()
        {
            for(int nCOunt = 0; nCOunt < Set_Count; nCOunt++)
            {
                Set[nCOunt].Set_Number = nCOunt;
                for(int nIndex = 0; nIndex < n_Set_Association; nIndex++)
                {

                    Set[nCOunt].Tag[nIndex]        = -1;
                    Set[nCOunt].Data[nIndex]       = -1;
                    Set[nCOunt].dirty[nIndex]      = false;
                    Set[nCOunt].valid[nIndex]    = true;
                }
            }
        };

        //This Function Check a given Set if there is the Tag for Cache Hit or Miss
        // Parameter:  *ChangedLine: Return Value of the Line Number which can be overwritten
        //             SetNumber : This Value should give the index number where the tag should be searched
        //             Tag: Adress Tag
        // Return:     true - there is a cache hit
        //             false - there is a cache miss
        bool CheckforMissorHit(short *ChangedLine,short SetNumber, int Tag, bool isFromWrite)
        {
            double ShortestTime = sc_time_stamp().to_double();
            for(short LineNumber = 0; LineNumber < n_Set_Association; LineNumber++)
            {
                if(Set[SetNumber].Tag[LineNumber] == Tag)
                {
                    /*if(Set[SetNumber].dirty[LineNumber] && isFromWrite)
                    {
                        //Cache miss - Cache Line is not written into Memory yet
                        *ChangedLine = LineNumber;
                        return false;
                    }*/
                    //else
                    {
                        //Cache hit- Tag is already in Cache and is synechronized with Memory
                        *ChangedLine = LineNumber;
                        return true;
                    }
                }
                if(Set[SetNumber].Tag[LineNumber] == -1 && isFromWrite)
                {
                    //Cache hit, Line is Empty
                    *ChangedLine = LineNumber;
                    cout << "Line Number : "<< LineNumber<< "  && "<< SetNumber<< "  ChangedLine" << *ChangedLine<<endl;
                    return true;
                }
                else
                {
                    if(Set[SetNumber].TimeStamp[LineNumber] < ShortestTime)
                    {
                        *ChangedLine = LineNumber;
                        ShortestTime = Set[SetNumber].TimeStamp[LineNumber];
                    }
                }

            }

            // Cache miss- no empty Line
            return false;
        };

        //This Function overwrite a Cache Line with the give Parameter
        void WriteLine( short SetNumber, short LineNumber, int Tag, int Data,bool valid , bool dirty = true)
        {
            Set[SetNumber].Tag[LineNumber]     = Tag;
            Set[SetNumber].Data[LineNumber]    = Data;
            Set[SetNumber].dirty[LineNumber]   = dirty;
            Set[SetNumber].valid[LineNumber] = valid;
            Set[SetNumber].TimeStamp[LineNumber] = sc_time_stamp().to_double();

        };

        int LinePosition (int SetNumber,int Tag)
        {
            for(short LineNumber = 0; LineNumber < n_Set_Association; LineNumber++)
            {
                if(Set[SetNumber].Tag[LineNumber] == Tag)
                    return LineNumber;
            }
            return -1;
        };


    };





    // Custom constructor
    Cache(sc_module_name nm, int pid): sc_module(nm), pid_(pid) {
        SC_THREAD(bus);
        SC_THREAD(execute);
        sensitive << Port_CLK.pos();
        // Perhaps dont_initialize() can be executed
        //dont_initialize();

        //logger << "[Cache" << pid_ << "][execute] " << "start" << endl;
        // Build an array of sets, where each set is an array of lines.
        // This have to be done in Constructor so the the Bus loop can also change the valid bit

    }

    /* Set Variable is Private Member of Cache, so different Threads can access it*/
    Cache_Sets set_;
private:
    int pid_;


    int getIndex (int address) {
        return (address & 0x0000FE0) >> 5;
    }

    int getTag (int address) {
        return (address & 0xFFFFF000) >> 12;
    }

    /* Thread that handles the bus. */
    void bus()
    {
        logger << "[Cache" << pid_ << "][bus] start" << endl;

        //int f = 0;

        /* Continue while snooping is activated. */
        while(true)
        {
            /* Wait for work. */
            wait(Port_BusValid.value_changed_event());
            logger << "[Cache" << pid_ << "][bus] noticed an event" << endl;

            /*When the same Core is snooping nothing have to do be done-> continue for next event */
            int BusCoreId = Port_BusWriter.read();
            if(pid_ == BusCoreId)
                continue;

            int addr   = Port_BusAddr.read().to_uint();
            int index  = getIndex(addr);
            int tag    = getTag(addr);
            int linePosition = set_.LinePosition(index, tag);

            // cout << "BUS Valid is :"<< set_[index].line[linePosition].valid <<" -Thread "<< pid_<< endl;

            /* If one Core, which is not in the Bus, Recieve this, looks for Possibilities*/
            switch(Port_BusValid.read())
            {
                case F_READ:    //For Valid-Invalid Protocol, every Possibility is the same
                    break;
                case F_WRITE:
                case F_INVALID:

                    set_.WriteLine(index, linePosition, tag, 0, false);
                   // logger <<"***************"<<endl<< "Bus Read/Write happen." << endl<<"This Core is "<<pid_<<" - index / line = "<<index<<" / "<<linePosition<<endl<<"  -Bus core is "<< Port_BusWriter.read()<<endl;
                    break;
            }
        }
    }

    void execute()
    {


        while (true)
        {
            wait(Port_CpuFunc.value_changed_event());	// this is fine since we use sc_buffer

            Function f = Port_CpuFunc.read();
            int addr   = Port_CpuAddr.read();
            int index  = getIndex(addr);
            int tag    = getTag(addr);
            int data   = 0;
            short ChangedLine = 0;

            //cout << "Index: " << index << "   Tag: " << tag << endl;
            //logger << "Index: " << index << "   Tag: " << tag << endl;

            Port_Index.write(index);
            Port_Tag.write(tag);

            if (f == F_WRITE)
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
           // cout << "CACHE Valid "<< set_[index].line[linePosition].valid << " -Core "<<pid_<< endl;
            if (f == F_READ) {

                bool b_CacheHit = set_.CheckforMissorHit(&ChangedLine, index, tag, false);

                if(b_CacheHit && set_.Set[index].valid)
                {
                    stats_readhit(pid_);
                    Port_HitMiss.write(true);
                    hitRate++;
                }
                else
                {
                    stats_readmiss(pid_);
                    Port_HitMiss.write(false);
                    missRate++;
                    Port_Bus->read(pid_,addr,false);
                    set_.WriteLine(index, ChangedLine, tag, data, true);
                }

                Port_CpuDone.write( RET_READ_DONE );

            }
            else //writing
            {
                bool b_CacheHit = set_.CheckforMissorHit(&ChangedLine, index, tag, true);

                if (b_CacheHit)
                {
                    if(set_.Set[index].valid)
                    {
                        stats_writehit(pid_);
                        Port_HitMiss.write(true);
                        hitRate++;
                        Port_Bus->write(pid_,addr,data);
                    }
                    else
                    {
                        stats_writemiss(pid_);
                        Port_HitMiss.write(false);
                        missRate++;
                        Port_Bus->read(pid_,addr,true);
                    }
                    set_.WriteLine(index, ChangedLine, tag,data,true);
                }
                else
                {
                    if(set_.Set[index].Tag[ChangedLine] != -1)
                    {
                        //no empty line --> write miss
                        stats_writemiss(pid_);
                        Port_HitMiss.write(false);
                        missRate++;
                    }
                    else
                    {
                        //empty line in Cache --> write hit
                        stats_writehit(pid_);
                        Port_HitMiss.write(true);
                        hitRate++;
                    }
                    Port_Bus->write(pid_,addr,data);
                    set_.WriteLine(index, ChangedLine, tag,data,true);
                }


                wait();
                //cout<< "writing CPU WRITING DONE: "<<endl;
                Port_CpuDone.write( RET_WRITE_DONE );
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
                    f = F_READ;
                    break;

                case TraceFile::ENTRY_TYPE_WRITE:
                    f = F_WRITE;
                    break;

                case TraceFile::ENTRY_TYPE_NOP:
                    f = F_INVALID;
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

                if (f == F_WRITE)
                {
                    //cout << sc_time_stamp() << ": [CPU" << pid_ << "] sends write" << endl;

                    uint32_t data = rand();
                    Port_CacheData.write(data);
                    wait();

                }
                else
                {
                    //cout << sc_time_stamp() << ": [CPU" << pid_ << "] sends read" << endl;
                }

                // cout << "waiting" << endl;
                wait(Port_CacheDone.value_changed_event());
                //  cout << "value changed" << endl;

                if (f == F_READ)
                {
                    //cout << sc_time_stamp() << ": [CPU" << pid_ << "] reads: " << Port_CacheData.read() << endl;
                }
            }
            else
            {
                //  cout << sc_time_stamp() << ": [CPU" << pid_ << "] executes NOP" << endl;
            }

            // chceck if end of file
            endOfFile = tracefile_ptr->eof();

            // Advance one cycle in simulated time
            wait();
            //cout << endl;
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
            //cout << "Total runtime: " << sc_time_stamp() << endl;
        }

    }
};

class ProcessingUnit : public sc_module
{
public:

    CPU *cpu;
    Cache *cache;

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
        char name_cpu[12];
        char name_cache[12];

        std::sprintf(name_cpu, "cpu_%d", pid);
        std::sprintf(name_cache, "cache_%d", pid);

        cpu = new CPU(name_cpu, pid_);

        cpu->Port_CacheFunc(sigCpuFunc);
        cpu->Port_CacheAddr(sigCpuAddr);
        cpu->Port_CacheData(sigCpuData);
        cpu->Port_CacheDone(sigCpuDone);
        cpu->Port_CLK(Port_CLK);
        logger << "[PU" << pid_ << "] cpu created" << endl;

        // Create and patch Cache
        cache = new Cache(name_cache, pid_);

        cache->Port_CpuFunc(sigCpuFunc);
        cache->Port_CpuAddr(sigCpuAddr);
        cache->Port_CpuData(sigCpuData);
        cache->Port_CpuDone(sigCpuDone);
        //    cache->Port_BusFunc(sigBusFunc);
        cache->Port_CLK(Port_CLK);
        // signals for output trace
        cache->Port_Index(sigIndex);
        cache->Port_Tag(sigTag);
        cache->Port_ReadWrite(sigReadWrite);
        cache->Port_HitMiss(sigHitMiss);

        logger << "[PU" << pid_ << "] cache created" << endl;


        SC_THREAD(execute);
        logger << "[PU" << pid_ << "] thread registered" << endl;
        sensitive << Port_CLK.pos();
        // perhaps dont_initialize() can be executed
        //dont_initialize();
    }

//protected:  //Only for VCD TraceFile purpose-> public
public:
    int pid_;


    sc_signal<int>        sigIndex;
    sc_signal<int>        sigTag;
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

        // Create sc_buffer for connection between bus and caches
        // sc_signal<int>        sigBusWriter;
        sc_buffer<int, SC_MANY_WRITERS >   sigBusWriter;
        sc_buffer<Function, SC_MANY_WRITERS> sigBusValid;

        // Create Bus
        Bus         bus("bus");
        bus.Port_CLK(clk);

        // General Port_BusBus Signals
        bus.Port_BusWriter(sigBusWriter);
        bus.Port_BusValid(sigBusValid);


        // Create a vector of pointers to processing units
        std::vector<ProcessingUnit*> processingUnits;

        for( int i = 0; i < num_procs; i++ )
        {
            // Create processing unit with given PID
            ProcessingUnit* processingUnit = new ProcessingUnit("pu", i);
            processingUnit->Port_CLK(clk);
            // try to patch Caches that are in PUs
            processingUnit->cache->Port_BusAddr(bus.Port_BusAddr);
            processingUnit->cache->Port_BusWriter(sigBusWriter);
            processingUnit->cache->Port_BusValid(sigBusValid);
            processingUnit->cache->Port_Bus(bus);
            // Push into vector
            processingUnits.push_back(processingUnit);
        }


        logger << "[main] "  << "processingUnits created" << endl;
        logger << "[main] "  << "processingUnits.size(): " << processingUnits.size() << endl;

        logger << "[main] " << "Processing unit patched with clock" << endl;

        hitRate = 0;
        missRate = 0;

        logger << "[main] " << "hitmissrate defined" << endl;


        cout << "Running (press CTRL+C to interrupt)... " << endl;

        /*
         * Comment out VCD Tracefile*/
        sc_trace_file *wf = sc_create_vcd_trace_file("cachehitmiss");
        sc_trace(wf,clk, "Clock");
        sc_trace(wf, bus.Port_BusAddr, "Bus/Address");
        sc_trace(wf, sigBusWriter, "Core/in/Bus");
        sc_trace(wf, sigBusValid, "Bus/Validr");
        for( int i = 0; i < num_procs; i++ ) {
            char name[12];
            std::sprintf(name, "Index_cpu_%d", i);
            sc_trace(wf, processingUnits[i]->sigIndex, name);
            char name1[12];
            std::sprintf(name1, "Hit_cpu_%d", i);
            sc_trace(wf, processingUnits[i]->sigHitMiss, name1);
            char name2[12];
            std::sprintf(name2, "Read_cpu_%d", i);
            sc_trace(wf, processingUnits[i]->sigReadWrite, name2);

        }

        // Start Simulation
        sc_start();

        // Print statistics after simulation finished
        stats_print();
        cout << endl;
        cout << "Avarage mem access time:" << (hitRate + missRate * 100) / (hitRate + missRate) << endl;
        cout << endl;

        // OutPut information about the Bus
        bus.output();


        //sc_close_vcd_trace_file(wf);
    }

    catch (exception& e)
    {
        cerr << e.what() << endl;
    }

    logger.close();
    return 0;
}
