`timescale 1ns/1ps

// One registered advisory candidate stage, NOT an order transmitter. Inputs
// must be a coherent snapshot in clk's domain; no CDC or MAC is provided.
// Downstream MUST revalidate generation/epoch, reserve risk atomically and
// apply current kill/packet integrity/sequence checks at actual transmit.
module quote_gate (
    input wire clk,
    input wire reset,
    input wire in_valid,
    input wire feed_ready,
    input wire packet_ok,
    input wire killed,
    input wire fresh,
    input wire [63:0] generation,
    input wire [63:0] risk_epoch,
    input wire [31:0] bid,
    input wire [31:0] ask,
    input wire signed [31:0] position,
    input wire [31:0] open_buy,
    input wire [31:0] open_sell,
    input wire [31:0] quantity,
    input wire [31:0] max_position,
    input wire [63:0] max_notional,
    output reg out_valid,
    output reg [63:0] out_generation,
    output reg [63:0] out_risk_epoch,
    output reg [31:0] out_bid,
    output reg [31:0] out_ask,
    output reg [31:0] out_quantity
);
    // Widen BEFORE arithmetic to avoid 32-bit wrap or unsigned short positions.
    wire signed [63:0] pos = {{32{position[31]}}, position};
    wire signed [63:0] buys = {32'b0, open_buy};
    wire signed [63:0] sells = {32'b0, open_sell};
    wire signed [63:0] qty = {32'b0, quantity};
    wire signed [63:0] limit_pos = {32'b0, max_position};
    wire [63:0] bid_notional = {32'b0, bid} * {32'b0, quantity};
    wire [63:0] ask_notional = {32'b0, ask} * {32'b0, quantity};
    wire allowed = feed_ready && packet_ok && !killed && fresh &&
        bid > 0 && ask > bid && ask <= 32'd1000000000 &&
        max_position > 0 && max_position <= 32'd1000000 &&
        quantity > 0 && quantity <= max_position && max_notional > 0 &&
        pos + buys + qty <= limit_pos && pos - sells - qty >= -limit_pos &&
        bid_notional <= max_notional && ask_notional <= max_notional;

    always @(posedge clk) begin
        if (reset) begin
            out_valid <= 0;
            out_generation <= 0;
            out_risk_epoch <= 0;
            out_bid <= 0;
            out_ask <= 0;
            out_quantity <= 0;
        end else begin
            out_valid <= in_valid && allowed;
            out_generation <= generation;
            out_risk_epoch <= risk_epoch;
            out_bid <= bid;
            out_ask <= ask;
            out_quantity <= quantity;
        end
    end
endmodule
