// Icarus Verilog bench for the emitted SystemVerilog of Smoother<5>: replays
// two of the tests in tests/smoother_test.tern against the generated RTL.
//   tern build && iverilog -g2012 -o tb.vvp -c build/default/Smoother_5.f sim/tb_smoother.sv && vvp tb.vvp
`timescale 1ns / 1ps
module tb_smoother;
    logic clk = 0, rst = 1;
    always #5 clk = ~clk;

    // the filter alone: impulse response
    logic        f_in_valid;
    logic [11:0] f_in_data;
    logic        f_out_valid;
    logic [11:0] f_out_data;
    Fir_12_5 fir (.clk(clk), .rst(rst), .in_valid(f_in_valid), .in_data(f_in_data),
                  .out_valid(f_out_valid), .out_data(f_out_data));

    // the datapath: warm-up, run, stop
    logic        start = 0, stop = 0;
    logic [3:0]  gain_sel = 4;
    logic        s_valid = 0;
    logic [11:0] s_data = 0;
    logic        o_valid;
    logic [11:0] o_data;
    logic [1:0]  mode;
    logic [15:0] crc;
    Smoother_5 dut (.clk(clk), .rst(rst), .start(start), .stop(stop), .gain_sel(gain_sel),
                    .samples_valid(s_valid), .samples_data(s_data),
                    .out_valid(o_valid), .out_data(o_data), .mode(mode), .crc(crc));

    integer errors = 0, i, outputs;
    integer want[0:6];

    task step(input integer n);
        repeat (n) @(posedge clk);
        #1;
    endtask

    task check(input logic cond, input [8*48-1:0] what);
        if (!cond) begin
            $display("FAIL: %0s", what);
            errors = errors + 1;
        end
    endtask

    initial begin
        f_in_valid = 0;
        f_in_data = 0;
        step(2);
        rst = 0;
        // impulse: 16 = the kernel's sum, so the response is 1 4 6 4 1
        f_in_valid = 1;
        f_in_data = 16;
        step(1);
        f_in_data = 0;
        want[0] = 0; want[1] = 1; want[2] = 4; want[3] = 6; want[4] = 4; want[5] = 1; want[6] = 0;
        for (i = 0; i < 7; i = i + 1) begin
            step(1);
            check(f_out_valid && f_out_data == want[i], "impulse response");
        end

        // run: warm-up for 5 samples, then a constant passes unchanged
        start = 1;
        step(1);
        start = 0;
        check(mode == 2'd1, "Warmup after start");
        s_valid = 1;
        s_data = 1000;
        step(5);
        check(mode == 2'd2, "Run after 5 samples");
        outputs = 0;
        for (i = 0; i < 20; i = i + 1) begin
            step(1);
            if (o_valid) begin
                check(o_data == 1000, "constant passes unchanged");
                outputs = outputs + 1;
            end
        end
        check(outputs >= 16, "one output per sample");
        stop = 1;
        step(1);
        stop = 0;
        check(mode == 2'd0, "Idle after stop");
        step(4);
        check(!o_valid, "no output when idle");

        if (errors == 0) $display("PASS");
        else $display("%0d failures", errors);
        $finish;
    end
endmodule
