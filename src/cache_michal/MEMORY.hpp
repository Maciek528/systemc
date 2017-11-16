

#include <bitset>
#include <vector>

static const int MEM_SIZE = 32;

static const int LINES_IN_SET = 8;
static const int SETS_IN_CACHE = 128;
static const int LINES_IN_CACHE = 1024;


struct CacheLine
  {
    int tag;
    int age;
  };
typedef std::vector<CacheLine> CacheSet;
typedef std::vector<CacheSet> CacheData;

SC_MODULE(Memory)
{

public:
  //typedef std::vector<CacheBlock> CacheWhole;

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

  sc_in<bool>       Port_CLK;
  sc_in<Function>   Port_Func;
  sc_in<unsigned>   Port_Addr;
  sc_out<RetCode>   Port_Done;
  sc_inout_rv<32>   Port_Data;

  // Constructor
  SC_CTOR(Memory)
  {
    SC_THREAD(execute);
    sensitive  << Port_CLK.pos();
    dont_initialize();


    dataLog.open("data.log", ios::trunc);
  }

  // Destructor
  ~Memory()
  {
    dataLog << "----------------" << endl;
    dataLog << "# reads: " << numReads << endl;
    dataLog << "# writes: " << numWrites << endl;
    dataLog.close();
  }


private:

  unsigned numReads = 0;
  unsigned numWrites = 0;

  // Data logging object
  ofstream dataLog;

  // set member variables for tag, index and offset
  int m_tag, m_index, m_offset;

  // initialize tag to some negative value and age some big positive value
  CacheLine m_line = {-1, 100};
  // initialize cache
  CacheSet m_set;
  CacheData m_cacheData;

  void initializeCache()
  {
    for(unsigned i=0; i < LINES_IN_SET; i++)
    {
      m_set.push_back(m_line);
    }
    for(unsigned i=0; i<SETS_IN_CACHE; i++)
    {
      m_cacheData.push_back(m_set);
    }
  };

  void incrementAllAges(CacheSet* set)
  {
    std::vector<CacheLine>::iterator it;
    for(it = set->begin(); it != set->end(); ++it)
    {
      it->age++;
    }
  };

  void incrementAllAgesBut(CacheSet* set, int lineNumber)
  {
    std::vector<CacheLine>::iterator it;
    for(it = set->begin(); it != set->end(); ++it)
    {
      if(it - set->begin() == lineNumber)
      {
        it->age = 0;
      }
      else
      {
        it->age++;
      }
    }
  };
  void printAges(CacheSet* set)
  {
    std::vector<CacheLine>::iterator it;
    for(it = set->begin(); it != set->end(); ++it)
    {
      dataLog << "\t age @ " << (it - set->begin()) <<  " | " << it->age << endl;
    }
  }

  void findOldestAndReplace(CacheSet* set, int req_tag)
  {
    std::vector<CacheLine>::iterator it;
    std::vector<CacheLine>::iterator oldest = set->begin();
    for(it = set->begin(); it != set->end(); ++it)
    {
      if(it->age > oldest->age)
      {
        dataLog << "\tage [" <<  it->age << "] @ line: " << (it - set->begin()) << " is greater that current oldest (" << oldest->age << ")" << endl;
        oldest = it;
      }
      else
      {
        it->age++;
      }
    }
    // set new tag and age
    oldest->tag = req_tag;
    oldest->age = 0;

    dataLog << "\twrote new tag at line # " << (oldest - set->begin()) << endl;

  };


  unsigned createMask(unsigned a, unsigned b)
  {
    unsigned r = 0;
    for(unsigned i=a; i<=b; i++)
    {
      r |= 1 << i;
    }
    return r;
  };

  void execute()
  {
    initializeCache();

    while(true)
    {
      // this below is fine since sc_buffer is used
      wait(Port_Func.value_changed_event());

      Function f    = Port_Func.read();
      unsigned addr = Port_Addr.read();
      int data      = 0;

      // TAG BITS [9-11]
      //m_tag     = (addr & createMask(9, 11) >> 9);
      m_tag     = (addr & createMask(12, 32) >> 12);
      // INDEX BITS [2-8]
      //m_index   = (addr & createMask(2, 8) >> 2);
      m_index     = (addr & createMask(5, 11) >> 5);
      // OFFSET BITS [0-1]
      //m_offset  = (addr & createMask(0, 1));
      m_offset    = (addr & createMask(0, 4));

      if( f == FUNC_WRITE )
      {
        numWrites++;
        cout << sc_time_stamp() << ": MEM received write" << endl;

        dataLog << "WRITE" << endl;
        dataLog << "\tset #" << m_index << endl;
        if(m_index > SETS_IN_CACHE)
        {
          cerr << "Index: " << m_index << " too big" << endl;
          dataLog << "!error Index: " << m_index << " too big" << endl;
        }

        CacheSet* set = &m_cacheData[m_index];

        data = Port_Data.read().to_int();

        // find if TAG is in block
        bool found = false;
        int lineNumber;
        dataLog << "\trequested TAG :" << m_tag << endl;

        std::vector<CacheLine>::iterator it;
        for(it = set->begin(); it != set->end(); ++it)
        {
          if(it->tag == m_tag)
          {
            found = true;
            lineNumber = it - set->begin();
          }
        }

        if(found)
        {
          // tag found, WRITE HIT
          cout << sc_time_stamp() << "[WRITE HIT] tag found" << endl;
          stats_writehit(0);
          dataLog << "\t\t[WRITE HIT] TAG found at line # " << lineNumber << endl;
          // 1. increment all other age-fields
          // 2. set the age-field of found entry to the original value (0)
          printAges(set);
          incrementAllAgesBut(set, lineNumber);
          printAges(set);
          wait();
        }
        else
        {
          // tag not found, the oldest entry should be replaced
          cout << sc_time_stamp() << "[WRITE MISS] tag not found" << endl;
          stats_writemiss(0);
          dataLog << "\t\t[WRITE MISS] requested tag not found" << endl;
          // 1. find the oldest entry and replace it
          printAges(set);
          findOldestAndReplace(set, m_tag);
          printAges(set);
          // 2. write the new data to the memory also
          wait(100);
        }

        Port_Done.write( RET_WRITE_DONE );
      }
      else
      {
        //cout << sc_time_stamp() << ": MEM received read" << endl;
      }

      if( f == FUNC_READ )
      {
        numReads++;
        cout << sc_time_stamp() << ": MEM received read" << endl;
        dataLog << "READ" << endl;
        dataLog << "\tset #" << m_index << endl;
        if(m_index > SETS_IN_CACHE)
        {
          cerr << "Index: " << m_index << " too big" << endl;
          dataLog << "!error Index: " << m_index << " too big" << endl;
        }

        CacheSet* set = &m_cacheData[m_index];

        // find if requested tag is in block
        bool found = false;
        int lineNumber;

        dataLog << "\trequested TAG: " << m_tag << endl;

        std::vector<CacheLine>::iterator it;
        for(it = set->begin(); it != set->end(); ++it)
        {
          if(it->tag == m_tag)
          {
            found = true;
            lineNumber = it - set->begin();
          }
        }

        if(found)
        {
          // tag found, READ HIT
          cout << sc_time_stamp() << "\tREAD HIT" << endl;
          stats_readhit(0);
          dataLog << "\t\t[READ HIT] requested tag found at line # " << lineNumber << endl;
          // 1. increment all other age-fields
          // 2. write the age-field to the original value (0)
          printAges(set);
          incrementAllAgesBut(set, lineNumber);
          printAges(set);
        }
        else
        {
          cout << sc_time_stamp() << "[READ MISS] requested tag not found" << endl;

          stats_readmiss(0);
          // tag not found, the oldest entry should be replaced
          dataLog << "\t\t[READ MISS] requested tag not found" << endl;
          // 1. take requested data from main memory
          wait(100);
          // 2. replace the oldest cache entry with that data
          printAges(set);
          findOldestAndReplace(set, m_tag);
          printAges(set);
        }

        Port_Done.write( RET_READ_DONE );

        wait();
        Port_Data.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
      }
      else
      {
        Port_Done.write( RET_WRITE_DONE );
      }

    } // end while

  } // end execute

};
