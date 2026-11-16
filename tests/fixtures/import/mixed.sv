package bus_pkg;
    typedef logic [31:0] word_t;
endpackage

interface axi_lite_if #(parameter int AW = 32);
    logic [AW-1:0] awaddr;
    modport master (output awaddr);
endinterface

// A PLL wrapper as in spec section 13.4.
module pll_wrapper (
    input  wire  refclk,
    input  wire  rst,
    output wire  outclk,
    output wire  locked
);
endmodule

module test #(
    parameter int unsigned WIDTH = 8,
    parameter DEPTH = 16,
    localparam int AW = $clog2(DEPTH),
    parameter logic [7:0] INIT = 8'hA5,
    parameter bit ENABLE = 1'b1,
    parameter signed OFFSET = -3,
    parameter type T = logic,
    parameter string NAME = "fifo",
    parameter real FREQ = 1.5e8
) (
    input  logic                 aclk,
    input  logic                 aresetn,
    input  logic                 clk_en,
    input  logic [WIDTH-1:0]     din, din2,
    input  logic [AW:0]          level,
    input  logic [3:0][7:0]      lanes,
    input  logic [0:7]           msb_first,
    output logic signed [WIDTH-1:0] dout,
    output logic [AW-1:0]        addr [0:3],
    inout  wire  [3:0]           gpio,
    inout  wire                  sda,
    input  logic                 on,
    input  bus_pkg::word_t       word,
    axi_lite_if.master           bus,
    input  int                   count,
    input  int unsigned          ucount,
    input  logic [`DMA_BURST-1:0] burst,
    output logic                 rst_done,
    input  wire                  sys_clk_p
);
endmodule

module empty_mod;
endmodule
