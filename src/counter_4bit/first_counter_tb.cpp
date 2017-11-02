#include "systemc.h"
#include "first_counter.cpp"

int sc_main (int argc, char* argv[]) {
  sc_clock clock("clk", sc_time(1, SC_NS));
  sc_signal<bool>   reset;
  sc_signal<bool>   enable;
  sc_signal<sc_uint<4> > counter_out;

  // Connect the DUT
  first_counter counter("COUNTER");
    counter.clock(clock);
    counter.reset(reset);
    counter.enable(enable);
    counter.counter_out(counter_out);

  // Open VCD file
  sc_trace_file *wf = sc_create_vcd_trace_file("counter");
  // Dump the desired signals
  sc_trace(wf, clock, "clock");
  sc_trace(wf, reset, "reset");
  sc_trace(wf, enable, "enable");
  sc_trace(wf, counter_out, "count");

  // Initialize all variables
  reset = 0;       // initial value of reset
  enable = 0;      // initial value of enable
  sc_start(4,SC_NS);

  reset = 1;    // Assert the reset
  cout << "@" << sc_time_stamp() <<" Asserting reset\n" << endl;
  sc_start(8,SC_NS);

  reset = 0;    // De-assert the reset
  cout << "@" << sc_time_stamp() <<" De-Asserting reset\n" << endl;
  sc_start(4,SC_NS);

  enable = 1;  // Assert enable
  cout << "@" << sc_time_stamp() <<" Asserting Enable\n" << endl;
  sc_start(24,SC_NS);

  cout << "@" << sc_time_stamp() <<" De-Asserting Enable\n" << endl;
  enable = 0; // De-assert enable
  sc_start(4,SC_NS);

  cout << "@" << sc_time_stamp() <<" Terminating simulation\n" << endl;
  sc_close_vcd_trace_file(wf);
  return 0;// Terminate simulation

 }
