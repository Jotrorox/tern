// Icarus Verilog bench for the emitted SystemVerilog of SensorBridge<4, 16>:
// replays "samples cross into the system domain in order" and "a full FIFO
// drops samples and counts them" from
// tests/sensor_bridge_test.tern against the generated RTL (100 MHz system
// clock, 25 MHz ADC clock).
//   tern build && iverilog -g2012 -o tb.vvp -c build/default/SensorBridge_4_16.f sim/tb_sensor_bridge.sv && vvp tb.vvp
`timescale 1ns / 1ps
module tb_sensor_bridge;
    logic sys_clk = 0, adc_clk = 0, ext_rst = 1;
    logic sys_run = 1;
    always #5 if (sys_run) sys_clk = ~sys_clk;
    always #20 adc_clk = ~adc_clk;

    logic        capture = 1, adc_valid = 0;
    logic [11:0] adc_data = 0;
    logic        avg_valid;
    logic [11:0] avg_data, peak;
    logic [15:0] count, dropped;

    SensorBridge_4_16 dut (.sys_clk(sys_clk), .adc_clk(adc_clk), .ext_rst(ext_rst), .capture(capture),
                           .adc_valid(adc_valid), .adc_data(adc_data), .avg_valid(avg_valid),
                           .avg_data(avg_data), .peak(peak), .count(count), .dropped(dropped));

    integer errors = 0, i;
    integer samples[0:3];

    initial begin
        samples[0] = 100; samples[1] = 200; samples[2] = 300; samples[3] = 400;
        #100 ext_rst = 0;
        #200;                                  // capture enable crosses (Sync2)
        for (i = 0; i < 4; i = i + 1) begin
            @(negedge adc_clk);
            adc_valid = 1;
            adc_data = samples[i];
            @(negedge adc_clk);
            adc_valid = 0;
        end
        #400;
        if (count != 4) begin $display("FAIL: count = %0d", count); errors = errors + 1; end
        if (peak != 400) begin $display("FAIL: peak = %0d", peak); errors = errors + 1; end
        if (dropped != 0) begin $display("FAIL: dropped = %0d", dropped); errors = errors + 1; end
        // stop the system clock: 20 samples into a 16-entry FIFO drop 4
        @(negedge sys_clk);
        sys_run = 0;
        for (i = 0; i < 20; i = i + 1) begin
            @(negedge adc_clk);
            adc_valid = 1;
            adc_data = i;
        end
        @(negedge adc_clk);
        adc_valid = 0;
        sys_run = 1;
        #1000;
        if (count != 20) begin $display("FAIL: count = %0d after the burst", count); errors = errors + 1; end
        if (dropped != 4) begin $display("FAIL: dropped = %0d after the burst", dropped); errors = errors + 1; end
        if (errors == 0) $display("PASS");
        else $display("%0d failures", errors);
        $finish;
    end
endmodule
