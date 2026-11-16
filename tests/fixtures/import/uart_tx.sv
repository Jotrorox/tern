`timescale 1ns / 1ps
// The SystemVerilog UART transmitter from spec chapter 2.
(* keep_hierarchy = "yes" *)
module uart_tx #(
    parameter int CLK_HZ = 50_000_000,
    parameter int BAUD = 115_200
) (
    input  logic       clk,
    input  logic       rst,
    input  logic       tx_valid,
    input  logic [7:0] tx_data,   /* payload */
    output logic       tx_ready,
    output logic       txd
);
    localparam int DIV = CLK_HZ / BAUD;
    localparam int TW = $clog2(DIV);
    typedef enum logic [1:0] {IDLE, START, DATA, STOP} state_t;
    state_t state;
    logic [TW-1:0] tick;
    always_comb begin
        case (state)
            IDLE, STOP: txd = 1'b1;
            default: txd = 1'b0;
        endcase
    end
    always_ff @(posedge clk) begin
        if (rst) state <= IDLE;
    end
    always @(*) begin end
endmodule
