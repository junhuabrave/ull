`timescale 1ns/1ps
module quote_gate_tb;
    reg clk = 0;
    always #5 clk = !clk;
    reg reset = 1, in_valid = 1, feed_ready = 1, packet_ok = 1, killed = 0, fresh = 1;
    reg [63:0] generation = 27, risk_epoch = 42, max_notional = 1040;
    reg [31:0] bid = 100, ask = 104, open_buy = 0, open_sell = 0, quantity = 10, max_position = 20;
    reg signed [31:0] position = 0;
    wire out_valid;
    wire [63:0] out_generation, out_risk_epoch;
    wire [31:0] out_bid, out_ask, out_quantity;
    quote_gate dut(.*);
    integer checks = 0;
    task check(input expected);
        begin
            @(posedge clk); #1;
            if (out_valid !== expected) $fatal(1, "case %0d: expected %0b got %0b", checks, expected, out_valid);
            if (!reset && (out_bid !== bid || out_ask !== ask || out_quantity !== quantity ||
                out_generation !== generation || out_risk_epoch !== risk_epoch))
                $fatal(1, "payload or version mismatch");
            checks = checks + 1;
            @(negedge clk);
        end
    endtask
    initial begin
        check(0); reset = 0;
        check(1); position = 10; check(1); position = 11; check(0);
        position = -10; check(1); position = -11; check(0);
        position = 3; open_buy = 7; check(1); open_buy = 8; check(0);
        position = -3; open_buy = 0; open_sell = 7; check(1); open_sell = 8; check(0);
        position = 0; open_sell = 0; ask = 105; check(0);
        ask = 104; killed = 1; check(0); killed = 0;
        fresh = 0; check(0); fresh = 1;
        feed_ready = 0; check(0); feed_ready = 1;
        packet_ok = 0; check(0); packet_ok = 1;
        in_valid = 0; check(0); in_valid = 1;
        ask = bid; check(0); ask = 104;
        quantity = 0; check(0); quantity = 10;
        open_buy = 32'hffffffff; check(0); open_buy = 0;
        open_sell = 32'hffffffff; check(0); open_sell = 0;
        bid = 999999999; ask = 1000000000; quantity = 1000000; max_position = 1000000;
        max_notional = 64'd1000000000000000; check(1);
        max_notional = 64'd999999999999999; check(0);
        generation = 28; risk_epoch = 43; max_notional = 64'd1000000000000000; check(1);
        reset = 1; check(0);
        $display("PASS: %0d RTL candidate gate boundary/reset/bubble checks", checks);
        $finish;
    end
endmodule
