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

void total_avg_print();

sc_mutex traceFileMtx;
sc_mutex doneProcessesMtx;

int numProcessesDone = 0;
int gNumProcesses;

static const int Cache_Size = 32768;    // Total L1 Cache Size is 32 KByte (only for data)
static const int Cache_Line = 32;       // Size of one cache Line is 32 byte
static const int Cache_Line_Count = Cache_Size / Cache_Line ; // number of total lines is 1024
static const int n_Set_Association = 8;  // The cache is 8 way set associative
static const int Set_Count = Cache_Line_Count / n_Set_Association ; // Number of sets in this cache is 128


// Function type
enum Function
{
    F_INVALID,
    F_READ,
    F_READEx,
    F_WRITE
};

enum Snooping
{
    Snoop_Hit = 0,
    Snoop_Miss = 1
};

enum RetCode
{
    RET_READ_DONE,
    RET_WRITE_DONE
};

enum State
{
    S_Modified  = 0,
    S_Owner     = 1,
    S_Exclusive = 2,
    S_Shared    = 3,
    S_Invalid   = 4,
    S_NotDefined= 5
};

struct stats
{
    int Reads;
    int RHit;
    int RMiss;
    int Writes;
    int WHit;
    int WMiss;
    double HitRate;
};

stats stats_total =
        {
                0,  0,  0,  0,  0,  0,  0.0
        };

double  hitRate,
        missRate,
        nSnoopHit,
        nSnoopMiss;

sc_event SnoopEvent;

// Initialize logger object
std::ofstream logger("logger.log", std::ios_base::out | std::ios_base::trunc);

//Class for holding information about the L1 D Cache
class Cache_L1{
public:
    typedef struct {
        int Set_Number;  // index number
        int Tag[n_Set_Association];
        int Data[n_Set_Association];
        /* The state variable stores the information for MOESI protocol
         * 0 - Modified
         * 1 - Owner
         * 2 - Exclusive
         * 3 - Shared
         * 4 - Invalid*/
        State state[n_Set_Association];
        double TimeStamp[n_Set_Association];
    } Cache_Set;


    Cache_Set Set[Set_Count];

    Cache_L1()
    {
        for(int nCOunt = 0; nCOunt < Set_Count; nCOunt++)
        {
            Set[nCOunt].Set_Number = nCOunt;
            for(int nIndex = 0; nIndex < n_Set_Association; nIndex++)
            {
                Set[nCOunt].Tag[nIndex]         = -1;
                Set[nCOunt].Data[nIndex]        = -1;
                Set[nCOunt].state[nIndex]       =  S_Invalid ; //At the begining every state is invalid
            }
        }
    };

    //This Function Check a given Set if there is the Tag for Cache Hit or Miss
    // Parameter:  *ChangedLine: Return Value of the Line Number which can be overwritten
    //             SetNumber : This Value should give the index number where the tag should be searched
    //             Tag: Adress Tag
    // Return:     true - there is a cache hit
    //             false - there is a cache miss
    bool CheckforMissorHit(short *ChangedLine,short SetNumber, int Tag, bool *HasMOState)
    {
        double ShortestTime = sc_time_stamp().to_double();
        *HasMOState = false;
        for(short LineNumber = 0; LineNumber < n_Set_Association; LineNumber++)
        {
            if(Set[SetNumber].Tag[LineNumber] == Tag)
            {
                *ChangedLine = LineNumber;
                if(Set[SetNumber].state[LineNumber] == S_Invalid) {
                    return false;
                } else
                {
                    return true;
                }
            }

            if(Set[SetNumber].TimeStamp[LineNumber] < ShortestTime)
            {
                *ChangedLine = LineNumber;
                ShortestTime = Set[SetNumber].TimeStamp[LineNumber];
                State osta = GetState(SetNumber, Tag);
                if(osta == S_Modified || osta == S_Owner)
                    *HasMOState = true;
            }

        }
        // Cache miss- no empty Line
        return false;
    };

    //This Function overwrite a Cache Line with the give Parameter
    void WriteLine( short SetNumber, short LineNumber, int Tag, int Data,State sta )
    {
        Set[SetNumber].Tag[LineNumber]      = Tag;
        Set[SetNumber].Data[LineNumber]     = Data;
        Set[SetNumber].state[LineNumber]    = sta;
        Set[SetNumber].TimeStamp[LineNumber] = sc_time_stamp().to_double();
    };

    void SetState(short SetNumber, int Tag, State sta )
    {
        int LineNumber = -1;
        for(short LineIndex = 0; LineIndex < n_Set_Association; LineIndex++)
        {
            if(Set[SetNumber].Tag[LineIndex] == Tag)
                LineNumber = LineIndex;
        }

        if(LineNumber == -1) {
            //   cout << "LINENUMBER is -1!!!"<<endl;
            return;
        }
        cout << "LINENUMBER is "<< LineNumber<<" !!!"<<endl;
        Set[SetNumber].state[LineNumber] = sta;
    };

    State GetState(short SetNumber, int tag)
    {
        int LineNumber = -1;
        for(short LineIndex = 0; LineIndex < n_Set_Association; LineIndex++)
        {
            if(Set[SetNumber].Tag[LineIndex] == tag)
                LineNumber = LineIndex;
        }
        if(LineNumber == -1) {
            //   cout << "LINENUMBER is -1!!!"<<endl;
            return S_Invalid;
        }
        return Set[SetNumber].state[LineNumber];
    }

};


// Simple Bus interface
class Bus_if : public virtual sc_interface
{
public:
    virtual bool BusRd(int writer, int addr) = 0;
    virtual bool BusRdX(int writer, int addr) = 0;
    virtual bool BusUpgr(int writer, int addr, int data) = 0;
    virtual void SetCacheState(int writer, State nSt) = 0;
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
    long *nBusRd;
    long *nBusRdX;
    long *nBusUpgr;
    short NumberofCores;

    State *CacheState;

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
       // nBusRd = 0;
       // nBusRdX = 0;
       // nBusUpgr = 0;
    }

    void SetNumberofCores(short nNumberofCOres)
    {
        NumberofCores = nNumberofCOres;
        CacheState = new State[NumberofCores];
        nBusRd = new long[NumberofCores];
        nBusRdX = new long[NumberofCores];
        nBusUpgr = new long[NumberofCores];
    }

    bool BusSnoop(int writer, int addr, Function CacheAction)
    {
        /* Try to get exclusive lock on bus. */
        logger<<endl<< "--> CORE "<< writer <<" try to Lock BUS for READ  -->"<<endl;
        while(busMtx.trylock() == -1){
            /* Wait when bus is in contention. */
            waits++;
            wait();
        }
        SetNotDefined(writer);
        cout<<"LOCKED"<< endl;

        /* Set lines. */
        Port_BusAddr.write(addr);
        Port_BusWriter.write(writer);
        switch(CacheAction)
        {
            case F_READ:
                cout<< "Cache Action Read"<<endl;
                Port_BusValid.write(F_READ);
                nBusRd[writer]++;
                break;
            case F_READEx:
                cout<< "Cache Action ReadEX"<<endl;
                Port_BusValid.write(F_READEx);
                nBusRdX[writer]++;
                break;
            case F_WRITE:
                cout<< "Cache Action Upgrd"<<endl;
                Port_BusValid.write(F_WRITE);
                nBusUpgr[writer]++;
                break;
            default:
                cout<< "Cache Action defualt"<<endl;
                break;
        }

        /* Wait for everyone to recieve. */
        bool bIsShared = false;
        bool bWaiting = true;
        while(bWaiting) {
            bWaiting = false;
            for (short nmrEvent = 0; nmrEvent < (NumberofCores); nmrEvent++) {
                if(CacheState[nmrEvent] == S_NotDefined)
                {
                    bWaiting = true;
                    wait();
                    break;
                }
                else if(CacheState[nmrEvent] != S_Invalid)
                {
                    bIsShared = true;
                    cout << "Set Shared true"<<endl;
                }
                wait();
            }
        }

        if(bIsShared) {
            wait(1);
            nSnoopHit++;
        }
        else {
            wait(100);
            nSnoopMiss++;
        }

        /* Reset. */
        Port_BusAddr.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
        busMtx.unlock();
        return(bIsShared);
    }

    /* Perform a Bus Read access to memory addr for CPU #writer. */
    virtual bool BusRd(int writer, int addr){
        return BusSnoop(writer, addr, F_READ);
    };

    /* Perform a Bus Read Exclusive access to memory addr for CPU #writer. */
    virtual bool BusRdX(int writer, int addr){
        return BusSnoop(writer, addr, F_READEx);
    };

    /* Write action to memory, need to know the writer, address and data. */
    virtual bool BusUpgr(int writer, int addr, int data){
        return BusSnoop(writer, addr, F_WRITE);
    }

    virtual bool Flush()
    {
        // wait(100);
        return false;
    }

    virtual void SetCacheState(int writer, State nSt)
    {
        // cout<< "Set Cache State"<< endl;
        CacheState[writer] = nSt;
    }

    void SetNotDefined(short writer)
    {
        for(short index=0; index < NumberofCores; index++)
        {
            CacheState[index] = S_NotDefined;
        }
        CacheState[writer] = S_Invalid;
    }

    /* Bus output. */
    void output(){
        /* Write output as specified in the assignment. */
        long nTot_BusRd = 0,
            nTot_BusRdX = 0,
            nTot_BusUpgr = 0;

        for(int index = 0; index < NumberofCores; index++)
        {
            nTot_BusRd = nTot_BusRd + nBusRd[index];
            nTot_BusRdX = nTot_BusRdX + nBusRdX[index];
            nTot_BusUpgr = nTot_BusUpgr + nBusUpgr[index];
        }

        double avg = (double)waits / double(nTot_BusRd + nTot_BusRdX + nTot_BusUpgr);
        printf("\n 1.    BusRd - BusRdEx - BusUpgr\n");
        for(int index = 0; index < NumberofCores; index++)
            printf("Core%d   %ld   %ld   %ld\n", index,nBusRd[index], nBusRdX[index],nBusUpgr[index]);
        printf("Total   %ld   %ld   %ld\n", nTot_BusRd, nTot_BusRdX, nTot_BusUpgr);
        printf("\n 2.    A total number of %ld accesses.\n", nTot_BusRd + nTot_BusRdX+ nTot_BusUpgr);
        printf("Snoop Hits:%.0f     Snoop Miss:%.0f\n", nSnoopHit, nSnoopMiss);
        printf("\n 3. Average time for bus acquisition\n");
        printf("    There were %ld waits for the bus.\n", waits);
        printf("    Average waiting time per access: %f cycles.\n", avg);
    }

    void WriteEvent()
    {
        cout<< "Writing SNoop Miss to Bus"<< endl;
        //Port_BusSnoop.write(Snoop_Miss);
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
    sc_in<int>          Port_BusWriter;
    sc_in<Function>     Port_BusValid;
    //sc_out<Snooping>    Port_Snoop;

    // Bus requests ports
    sc_port<Bus_if, 0>     Port_Bus;

    // Ports (tracefile)
    sc_out<int>        Port_Index;
    sc_out<int>        Port_Tag;
    sc_out<bool>       Port_ReadWrite;
    sc_out<bool>       Port_HitMiss;


    // has to be added when no standard constructor SC_CTOR is used
    SC_HAS_PROCESS(Cache);

    // Custom constructor
    Cache(sc_module_name nm, int pid): sc_module(nm), pid_(pid) {
        SC_THREAD(bus);
        SC_THREAD(execute);
        sensitive << Port_CLK.pos();
    }


private:
    /* Core Number*/
    int pid_;
    /* Set Variable is Private Member of Cache, so different Threads can access it*/
    Cache_L1 set_;

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

        /* Continue while snooping is activated. */
        while(true)
        {
            /* Wait for work. */
            wait(Port_BusValid.value_changed_event());
            cout << "[Cache" << pid_ << "][bus] noticed an event" << endl;

            /*When the same Core is snooping nothing have to do be done-> continue for next event */
            int BusCoreId = Port_BusWriter.read();
            if(pid_ == BusCoreId)
                continue;

            int addr   = Port_BusAddr.read().to_uint();
            int index  = getIndex(addr);
            int tag    = getTag(addr);
            State ActualState = set_.GetState(index,tag);

            Port_Bus->SetCacheState(pid_, ActualState);

            if( ActualState == S_Invalid)
            {
                cout<<"writting to bus Snoop_Miss"<<endl;
                continue;
            }


            /* If one Core, which is not in the Bus, Recieve this, looks for Possibilities*/
            switch(Port_BusValid.read())
            {
                case F_READ:   //BusRd
                    if(ActualState == S_Modified ||
                       ActualState == S_Exclusive ||
                       ActualState == S_Owner)
                    {
                        set_.SetState(index, tag, S_Owner);
                    }
                    break;
                case F_READEx:  //BusRdX
                case F_WRITE:   //BusUpgr
                    set_.SetState(index, tag , S_Invalid);
                    break;
                default:
                    break;
            }

            cout<<"writting to bus Snoop_Hit"<<endl;

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
                bool BusRdX = false;
                bool b_CacheHit = set_.CheckforMissorHit(&ChangedLine, index, tag, &BusRdX);

                if(b_CacheHit)
                {
                    stats_readhit(pid_);
                    stats_total.RHit++;
                    Port_HitMiss.write(true);
                    hitRate++;

                }
                else
                {
                    stats_readmiss(pid_);
                    stats_total.RMiss++;
                    Port_HitMiss.write(false);
                    missRate++;
                    if(Port_Bus->BusRd(pid_,addr))
                        set_.WriteLine(index, ChangedLine, tag, data, S_Shared);
                    else
                        set_.WriteLine(index, ChangedLine, tag, data, S_Exclusive);

                }

                Port_CpuDone.write( RET_READ_DONE );

            }
            else //writing
            {
                bool BusRdX = false;
                bool b_CacheHit = set_.CheckforMissorHit(&ChangedLine, index, tag, &BusRdX);

                if (b_CacheHit)
                {
                    stats_writehit(pid_);
                    stats_total.WHit++;
                    Port_HitMiss.write(true);
                    hitRate++;

                    switch (set_.Set[index].state[ChangedLine])
                    {
                        case S_Owner:
                        case S_Shared:
                            Port_Bus->BusUpgr(pid_,addr,data);
                        default:
                            break;
                    }
                    set_.WriteLine(index, ChangedLine, tag,data,S_Modified);

                }
                else
                {
                    stats_writemiss(pid_);
                    stats_total.WMiss++;
                    Port_HitMiss.write(false);
                    missRate++;
                    set_.WriteLine(index, ChangedLine, tag,data,S_Modified);
                    Port_Bus->BusRdX(pid_,addr);
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

        isDone_ = false;

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
                    // cout << sc_time_stamp() << ": [CPU" << pid_ << "] reads: " << Port_CacheData.read() << endl;
                }
            }
            else
            {
                //cout << sc_time_stamp() << ": [CPU" << pid_ << "] executes NOP" << endl;
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

protected:  //Only for VCD TraceFile purpose-> public

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
        bus.SetNumberofCores(num_procs);
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
        nSnoopHit = 0;
        nSnoopMiss = 0;

        logger << "[main] " << "hitmissrate defined" << endl;

        cout << "Running (press CTRL+C to interrupt)... " << endl;

        /*
         * Comment out VCD Tracefile*/
        /*  sc_trace_file *wf = sc_create_vcd_trace_file("cachehitmiss");
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

          }*/

        // Start Simulation
        sc_start();

        // Print statistics after simulation finished
        stats_print();
        total_avg_print();
        cout << endl;

        // OutPut information about the Bus
        bus.output();
        cout << endl <<"4. Avarage mem access time:" << (hitRate + nSnoopHit + (missRate - nSnoopHit) * 100) / (hitRate + missRate) << endl;
        cout << endl<< "5. Total execution time: "<< sc_time_stamp()<<endl;

        //sc_close_vcd_trace_file(wf);
    }

    catch (exception& e)
    {
        cerr << e.what() << endl;
    }

    logger.close();
    return 0;
}


void total_avg_print()
{
    printf("\n");

    stats_total.Reads   = stats_total.RHit + stats_total.RMiss;
    stats_total.Writes  = stats_total.WHit + stats_total.WMiss;
    stats_total.HitRate =  ((double) stats_total.RHit + (double) stats_total.WHit) / (double) (stats_total.Reads + stats_total.Writes);

    printf("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%f\n", "TOT: ",
           stats_total.Reads,
           stats_total.RHit,
           stats_total.RMiss,
           stats_total.Writes,
           stats_total.WHit,
           stats_total.WMiss,
           stats_total.HitRate);

    printf("%s\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%f\n", "AVG: ",
           (double) stats_total.Reads    / (double) num_cpus,
           (double) stats_total.RHit     / (double) num_cpus,
           (double) stats_total.RMiss    / (double) num_cpus,
           (double) stats_total.Writes   / (double) num_cpus,
           (double) stats_total.WHit     / (double) num_cpus,
           (double) stats_total.WMiss    / (double) num_cpus,
           (double) stats_total.HitRate  / (double) num_cpus);

}
