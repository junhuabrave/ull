`timescale 1ns/1ps

package itch_pkg;
    // Packed in network/display order. Matches the fields materialized by
    // include/ull/nasdaq/itch.hpp, plus the Mold channel sequence (not book epoch).
    typedef struct packed {
        logic [63:0] sequence_number;
        logic [7:0] message_type;
        logic [15:0] locate;
        logic [47:0] timestamp_ns;
        logic [63:0] stock;
        logic [63:0] reference_number;
        logic [63:0] replacement;
        logic [31:0] shares;
        logic [31:0] price4;
        logic [7:0] side;
        logic [7:0] trading_state;
        logic [7:0] market;
        logic [31:0] attribution;
    } itch_event_t;

    typedef struct packed {
        logic valid;
        itch_event_t data;
    } itch_decoded_t;

    typedef enum logic [3:0] {
        DATA, DUPLICATE, HEARTBEAT, END_SESSION, MALFORMED, SESSION_MISMATCH,
        GAP, DISARMED, ITCH_ERROR, CAPACITY, INTEGRITY
    } ingress_status_t;

    function automatic logic [15:0] itch_length(input logic [7:0] t);
        case (t)
            "S", "W": itch_length = 12;
            "R": itch_length = 39;
            "H": itch_length = 25;
            "Y", "N": itch_length = 20;
            "L": itch_length = 26;
            "V", "J", "U": itch_length = 35;
            "K": itch_length = 28;
            "h": itch_length = 21;
            "A", "C": itch_length = 36;
            "F", "Q": itch_length = 40;
            "E": itch_length = 31;
            "X": itch_length = 23;
            "D", "B": itch_length = 19;
            "P": itch_length = 44;
            "I": itch_length = 50;
            "O": itch_length = 48;
            default: itch_length = 0;
        endcase
    endfunction

    function automatic logic symbol_valid(input logic [63:0] symbol);
        logic padding;
        logic [7:0] c;
        padding = 0;
        symbol_valid = symbol[63:56] != " ";
        for (int i = 0; i < 8; i = i + 1) begin
            c = symbol[63 - i*8 -: 8];
            if (c == " ") padding = 1;
            else if (padding || c < 33 || c > 126) symbol_valid = 0;
        end
    endfunction

    // Byte zero occupies raw[399:392]; unused tail bytes are zero. Exact lengths
    // and consumed fields follow the C++ reference, including known non-book
    // types whose payloads are only length/header checked. This is NOT a book.
    function automatic itch_decoded_t decode_itch(input logic [399:0] raw,
                                                  input logic [15:0] length);
        itch_decoded_t d;
        logic [7:0] t, state_byte;
        logic global_message;
        // Tracking number and unmaterialized suffix are intentionally ignored,
        // just as in the C++ reference; keep this explicit for RTL lint.
        logic [95:0] unused_fields;
        unused_fields = {raw[375:360], raw[79:0]};
        d = '0;
        t = raw[399 - 0*8 -: 8];
        d.data.message_type = t;
        d.data.locate = raw[399 - 1*8 -: 16];
        d.data.timestamp_ns = raw[399 - 5*8 -: 48];
        d.data.attribution = "NSDQ";
        global_message = t == "S" || t == "V" || t == "W" || t == "K";
        d.valid = itch_length(t) != 0 && itch_length(t) == length &&
            d.data.timestamp_ns < 48'd86400000000000 &&
            global_message == (d.data.locate == 0);
        state_byte = raw[399 - 19*8 -: 8];
        case (t)
            "S", "W": begin
                state_byte = raw[399 - 11*8 -: 8];
                d.data.trading_state = state_byte;
                if (t == "S")
                    d.valid = d.valid && (state_byte == "O" || state_byte == "S" ||
                        state_byte == "Q" || state_byte == "M" || state_byte == "E" || state_byte == "C");
                else d.valid = d.valid && (state_byte == "1" || state_byte == "2" || state_byte == "3");
            end
            "R", "H", "Y", "h": begin
                d.data.stock = raw[399 - 11*8 -: 64];
                d.valid = d.valid && symbol_valid(d.data.stock);
                if (t == "R") d.valid = d.valid && raw[399 - 21*8 -: 32] != 0;
                else begin
                    d.data.trading_state = state_byte;
                    if (t == "H") d.valid = d.valid &&
                        (state_byte == "H" || state_byte == "P" || state_byte == "Q" || state_byte == "T");
                    if (t == "Y") d.valid = d.valid &&
                        (state_byte == "0" || state_byte == "1" || state_byte == "2");
                    if (t == "h") begin
                        d.data.market = state_byte;
                        d.data.trading_state = raw[399 - 20*8 -: 8];
                        d.valid = d.valid && (state_byte == "Q" || state_byte == "B" || state_byte == "X") &&
                            (d.data.trading_state == "H" || d.data.trading_state == "T");
                    end
                end
            end
            "A", "F": begin
                d.data.reference_number = raw[399 - 11*8 -: 64];
                d.data.side = state_byte;
                d.data.shares = raw[399 - 20*8 -: 32];
                d.data.stock = raw[399 - 24*8 -: 64];
                d.data.price4 = raw[399 - 32*8 -: 32];
                d.valid = d.valid && symbol_valid(d.data.stock) && (state_byte == "B" || state_byte == "S");
                if (t == "F") d.data.attribution = raw[399 - 36*8 -: 32];
            end
            "E", "C", "X": begin
                d.data.reference_number = raw[399 - 11*8 -: 64];
                d.data.shares = raw[399 - 19*8 -: 32];
                if (t == "C") begin
                    d.data.price4 = raw[399 - 32*8 -: 32];
                    d.valid = d.valid && (raw[399 - 31*8 -: 8] == "Y" || raw[399 - 31*8 -: 8] == "N");
                end
            end
            "D": d.data.reference_number = raw[399 - 11*8 -: 64];
            "U": begin
                d.data.reference_number = raw[399 - 11*8 -: 64];
                d.data.replacement = raw[399 - 19*8 -: 64];
                d.data.shares = raw[399 - 27*8 -: 32];
                d.data.price4 = raw[399 - 31*8 -: 32];
                d.valid = d.valid && d.data.replacement != 0 && d.data.replacement != d.data.reference_number;
            end
            default: begin end
        endcase
        if (t == "A" || t == "F" || t == "E" || t == "C" || t == "X" || t == "D" || t == "U")
            d.valid = d.valid && d.data.reference_number != 0;
        if (t == "A" || t == "F" || t == "E" || t == "C" || t == "X" || t == "U")
            d.valid = d.valid && d.data.shares != 0;
        if (t == "A" || t == "F" || t == "C" || t == "U")
            d.valid = d.valid && d.data.price4 != 0 && d.data.price4 <= 32'd2000000000;
        return d;
    endfunction
endpackage
