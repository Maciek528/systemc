

#include <bitset>
#include <vector>

static const int MEM_SIZE = 32;

static const int LINES_IN_BLOCK = 8;
static const int BLOCKS_IN_CACHE = 1024;


struct CacheLine
  {
    int tag;
    int age;
  };
typedef std::vector<CacheLine> CacheBlock;
typedef std::vector<CacheBlock> CacheData;

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

    dataLog.close();
  }


private:

  // Data logging object
  ofstream dataLog;

  // set member variables for tag, index and offset
  int m_tag, m_index, m_offset;

  // initialize tag to some negative value and age some big positive value
  CacheLine m_line = {-1, 100};
  // initialize cache
  CacheBlock m_block;
  CacheData m_cacheData;

  void initializeCache()
  {
    for(unsigned i=0; i<8; i++)
    {
      m_block.push_back(m_line);
    }
    for(unsigned i=0; i<128; i++)
    {
      m_cacheData.push_back(m_block);
    }
  };

  void incrementAllAgesBut(CacheBlock* block, int lineNumber)
  {
    std::vector<CacheLine>::iterator it;
    for(it = block->begin(); it != block->end(); ++it)
    {
      if(it - block->begin() == lineNumber)
      {
        it->age = 0;
      }
      else
      {
        it->age++;
      }
    }
  };

  void findOldestAndReplace(CacheBlock* block, int req_tag)
  {
    std::vector<CacheLine>::iterator it;
    std::vector<CacheLine>::iterator oldest = block->end();
    for(it = block->begin(); it != block->end(); ++it)
    {
      if(it->age > oldest->age)
      {
        oldest = it;
      }
    }
    // set new tag and age
    oldest->tag = req_tag;
    oldest->age = 0;

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
      m_tag     = (addr & createMask(9, 11) >> 9);
      // INDEX BITS [2-8]
      m_index   = (addr & createMask(2, 8) >> 2);
      // OFFSET BITS [0-1]
      m_offset  = (addr & createMask(0, 1));


      if( f == FUNC_WRITE )
      {
        cout << sc_time_stamp() << ": MEM received write" << endl;

        dataLog << "WRITE" << endl;
        dataLog << "\t block #" << m_index << endl;

        CacheBlock* block = &m_cacheData[m_index];

        data = Port_Data.read().to_int();

        // find if TAG is in block
        bool found = false;
        int lineNumber;
        dataLog << "requested TAG :" << m_tag << endl;

        std::vector<CacheLine>::iterator it;
        for(it = block->begin(); it != block->end(); ++it)
        {
          if(it->tag == m_tag)
          {
            found = true;
            lineNumber = it - block->begin();
          }
        }

        if(found)
        {
          // tag found, WRITE HIT
          cout << sc_time_stamp() << " [WRITE HIT] tag found" << endl;
          stats_readhit(0);
          dataLog << "\t\t [WRITE HIT] TAG found at line # " << lineNumber << endl;
          // 1. increment all other age-fields
          // 2. set the age-field of found entry to the original value (0)
          incrementAllAgesBut(block, lineNumber);
          wait();
        }
        else
        {
          // tag not found, the oldest entry should be replaced
          cout << sc_time_stamp() << " [WRITE MISS] tag not found" << endl;
          stats_readmiss(0);
          dataLog << "\t\t [WRITE MISS] requested tag not found" << endl;
          // 1. find the oldest entry and replace it
          findOldestAndReplace(block, m_tag);
          // 2. write the new data to the memory also
          wait(100);
        }

        Port_Done.write( RET_WRITE_DONE );
      }
      else
      {
        cout << sc_time_stamp() << ": MEM received read" << endl;
      }

      if( f == FUNC_READ )
      {
        cout << sc_time_stamp() << ": MEM received read" << endl;
        dataLog << "READ" << endl;
        dataLog << "\t block #" << m_index << endl;
        CacheBlock* block = &m_cacheData[m_index];

        // find if requested tag is in block
        bool found = false;
        int lineNumber;

        dataLog << "requested TAG: " << m_tag << endl;

        std::vector<CacheLine>::iterator it;
        for(it = block->begin(); it != block->end(); ++it)
        {
          if(it->tag == m_tag)
          {
            found = true;
            lineNumber = it - block->begin();
          }
        }

        if(found)
        {
          // tag found, READ HIT
          cout << sc_time_stamp() << "\t READ HIT" << endl;
          stats_writehit(0);
          dataLog << "\t\t [READ HIT] requested tag found at line # " << lineNumber << endl;
          // 1. increment all other age-fields
          // 2. write the age-field to the original value (0)
          incrementAllAgesBut(block, lineNumber);
        }
        else
        {
          cout << sc_time_stamp() << "[READ MISS] requested tag not found" << endl;

          stats_writemiss(0);
          // tag not found, the oldest entry should be replaced
          dataLog << "\t\t [READ MISS]" << endl;
          // 1. take requested data from main memory
          wait(100);
          // 2. replace the oldest cache entry with that data
          findOldestAndReplace(block, m_tag);
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
