// Verilog-2005, non-ANSI ports, parameters in the body.
`define DMA_BURST 16
`include "dma_defs.vh"

module legacy_dma (clk, rst_n, start, src, dst, len, busy, done, data_out, irq);
    parameter AW = 32;
    parameter integer DW = 64;
    parameter [3:0] CHANNELS = 4'd4;
    localparam BEATS = DW / 8;

    input clk;
    input rst_n;
    input start;
    input [AW-1:0] src, dst;
    input wire signed [15:0] len;
    output busy;
    output reg done;
    output [DW-1:0] data_out;
`ifdef DMA_IRQ
    output irq;
`else
    output irq;
`endif

    reg busy;
    reg [BEATS-1:0] beat;

    function [7:0] next_beat;
        input [7:0] cur;
        begin
            next_beat = cur + 1;
        end
    endfunction

    task automatic kick(input integer n);
        begin end
    endtask

    always @(posedge clk or negedge rst_n)
        if (!rst_n) done <= 1'b0;
endmodule
