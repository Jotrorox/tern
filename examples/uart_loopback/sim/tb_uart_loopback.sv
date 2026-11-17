// Icarus Verilog bench for the emitted SystemVerilog of UartLoopback<16>:
// replays "a byte received on rxd is echoed on txd at the programmed baud
// rate" from tests/uart_loopback_test.tern against the generated RTL.
//   tern build && iverilog -g2012 -o tb.vvp -c build/default/UartLoopback_16.f sim/tb_uart_loopback.sv && vvp tb.vvp
`timescale 1ns / 1ps
module tb_uart_loopback;
    logic clk = 0, rst = 1;
    always #5 clk = ~clk;

    logic        aw_valid = 0, w_valid = 0, b_ready = 1, ar_valid = 0, r_ready = 1;
    logic [31:0] aw_data = 0, ar_data = 0;
    logic [35:0] w_data = 0;          // {data, strb}
    logic        aw_ready, w_ready, b_valid, ar_ready, r_valid;
    logic [1:0]  b_data;
    logic [33:0] r_data;              // {data, resp}
    logic        rxd = 1, txd, irq, led;

    UartLoopback_16 dut (
        .clk(clk), .rst(rst),
        .bus_aw_valid(aw_valid), .bus_aw_data(aw_data), .bus_aw_ready(aw_ready),
        .bus_w_valid(w_valid), .bus_w_data(w_data), .bus_w_ready(w_ready),
        .bus_b_valid(b_valid), .bus_b_data(b_data), .bus_b_ready(b_ready),
        .bus_ar_valid(ar_valid), .bus_ar_data(ar_data), .bus_ar_ready(ar_ready),
        .bus_r_valid(r_valid), .bus_r_data(r_data), .bus_r_ready(r_ready),
        .rxd(rxd), .txd(txd), .irq(irq), .led(led));

    integer errors = 0, i, n;
    logic [7:0] v = 8'hA5;
    logic [9:0] frame;

    task step(input integer k);
        repeat (k) @(posedge clk);
        #1;
    endtask

    initial begin
        step(2);
        rst = 0;
        // baud divider: 8 cycles per bit
        aw_valid = 1; aw_data = 32'h04;
        w_valid = 1; w_data = {32'd8, 4'b1111};
        step(1);
        aw_valid = 0; w_valid = 0;
        n = 0;
        while (!b_valid && n < 10) begin step(1); n = n + 1; end
        if (!b_valid || b_data != 2'b00) begin $display("FAIL: write response"); errors = errors + 1; end
        step(1);
        // one 8N1 frame on rxd
        frame = {1'b1, v, 1'b0};
        for (i = 0; i < 10; i = i + 1) begin
            rxd = frame[i];
            step(8);
        end
        // the echo: start bit, 8 data bits LSB first, stop bit
        n = 0;
        while (txd && n < 64) begin step(1); n = n + 1; end
        step(4);
        if (txd) begin $display("FAIL: start bit"); errors = errors + 1; end
        for (i = 0; i < 8; i = i + 1) begin
            step(8);
            if (txd != v[i]) begin $display("FAIL: data bit %0d", i); errors = errors + 1; end
        end
        step(8);
        if (!txd) begin $display("FAIL: stop bit"); errors = errors + 1; end
        if (errors == 0) $display("PASS");
        else $display("%0d failures", errors);
        $finish;
    end
endmodule
