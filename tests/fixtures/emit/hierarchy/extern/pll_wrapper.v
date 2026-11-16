// Stand-in for the extern module of the hierarchy fixture (tests and lint only).
module pll_wrapper #(parameter DIV = 1) (
    input  wire refclk,
    input  wire rst,
    output wire outclk,
    output wire locked
);
    assign outclk = refclk;
    assign locked = !rst;
endmodule
