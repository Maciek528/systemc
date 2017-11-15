SC_MODULE(CPU)
{
public:
  sc_in<bool>                 Port_CLK;
  sc_in<Memory::RetCode>      Port_MemDone;
  sc_out<Memory::Function>    Port_MemFunc;
  sc_out<unsigned>                 Port_MemAddr;
  sc_inout_rv<32>             Port_MemData;

  // Constructor
  SC_CTOR(CPU)
  {
    SC_THREAD(execute);
    sensitive << Port_CLK.pos();
    dont_initialize();
  }

private:
  void execute()
  {
    TraceFile::Entry        tr_data;
    Memory::Function        f;

    //  Loop until end of tracefile
    while(!tracefile_ptr->eof())
    {

      // Get the next action for the processor in the trace
      while(!tracefile_ptr->next(0, tr_data))
      {
        cerr << "Error reading trace for CPU" << endl;
        break;
      }

      // To demonstrate the statistic function, we generate
      // a 50% probability of a 'hit' or 'miss', and call the statistic
      // functions below

      //int j = rand()%2;

      switch(tr_data.type)
      {
        case TraceFile::ENTRY_TYPE_READ:
        f = Memory::FUNC_READ;
        /*
        if(j)
        stats_readhit(0);
        else
        stats_readmiss(0);
        */

        break;

        case TraceFile::ENTRY_TYPE_WRITE:
        f = Memory::FUNC_WRITE;
        /*
        if(j)
        stats_writehit(0);
        else
        stats_writemiss(0);
        */
        break;

        case TraceFile::ENTRY_TYPE_NOP:
        break;

        default:
        cerr << "Error, got invalid data from Trace" << endl;
        exit(0);
      }

      if(tr_data.type != TraceFile::ENTRY_TYPE_NOP)
      {
        Port_MemAddr.write(tr_data.addr);
        Port_MemFunc.write(f);

        if(f == Memory::FUNC_WRITE)
        {
          cout << sc_time_stamp() << ": CPU sends write" << endl;

          uint32_t data = rand();
          Port_MemData.write(data);
          wait();
          Port_MemData.write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
        }
        else
        {
          cout << sc_time_stamp() << ": CPU sends read" << endl;
        }

        wait(Port_MemDone.value_changed_event());

        if(f == Memory::FUNC_READ)
        {
          cout << sc_time_stamp() << ": CPU reads: " << Port_MemData.read() << endl;
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
