`timescale 1ns/1ps

// Streaming UDP-PAYLOAD reference, not a MAC/IP/UDP receiver. Lane 0 is data[7:0].
// Each non-final beat has keep=ff; final keep is a nonzero low contiguous mask.
// integrity_ok is supplied by the upstream integrity checker on the final beat.
// Input/output transfers occur ONLY on valid && ready. One clock domain.
//
// Parse eight bytes per accepted beat, staging new events until the ENTIRE packet
// validates. Then drain events in sequence; input is backpressured during drain.
// This single-bank design is not a sustained-line-rate production receiver.
// A real MAC cannot backpressure the wire: integration needs buffering/overflow
// handling, a receive watchdog and book/risk invalidation on lost traffic.
module mold_itch_ingress #(
    parameter integer MAX_PACKET_BYTES = 2048,
    parameter integer MAX_EVENTS = 128
) (
    input logic clk,
    input logic reset,
    // Explicit trusted recovery checkpoint. Reset/arm abort partial input and
    // queued output; caller must also discard the rest of any upstream packet.
    input logic arm,
    input logic [79:0] arm_session,
    input logic [63:0] arm_next_sequence,
    input logic [63:0] s_data,
    input logic [7:0] s_keep,
    input logic s_valid,
    output logic s_ready,
    input logic s_last,
    input logic s_integrity_ok,
    output logic m_valid,
    input logic m_ready,
    output itch_pkg::itch_event_t m_event,
    output logic armed,
    output logic [63:0] next_sequence,
    // One-cycle notification on accepted EOF, independent of output draining.
    output logic status_valid,
    output itch_pkg::ingress_status_t status
);
    import itch_pkg::*;
    typedef enum logic [2:0] {HEADER, LENGTH_HI, LENGTH_LO, BODY, FINISHED} phase_t;
    typedef struct packed {
        phase_t phase;
        logic [4:0] header_at;
        logic [79:0] session_id;
        logic [63:0] sequence_number;
        logic [15:0] count;
        logic [15:0] message_index;
        logic [15:0] length;
        logic [15:0] body_at;
        logic [399:0] raw;
        logic [31:0] bytes_seen;
        logic [31:0] events;
        logic framing_bad;
        logic itch_bad;
        logic capacity_bad;
        logic decode_new;
    } parser_t;
    parser_t q, p;
    logic [79:0] expected_session;
    itch_event_t events [0:MAX_EVENTS-1];
    logic draining;
    logic [31:0] read_at, queued;
    logic write_event;
    localparam integer EVENT_ADDR_WIDTH = MAX_EVENTS > 1 ? $clog2(MAX_EVENTS) : 1;
    logic [EVENT_ADDR_WIDTH-1:0] write_at;
    itch_event_t staged_event;
    ingress_status_t result;
    logic [64:0] end_sequence;
    logic [399:0] raw_work;
    logic [7:0] byte_value;
    logic select_message;
    logic completed;
    logic [399:0] completed_raw;
    logic [15:0] completed_length;
    logic [63:0] completed_sequence;
    itch_decoded_t decoded;

    assign s_ready = !draining && !reset && !arm;
    assign m_valid = draining && !reset && !arm;
    assign m_event = m_valid ? events[read_at] : '0;

    always @* begin
        p = q;
        raw_work = q.raw;
        write_event = 0;
        write_at = 0;
        staged_event = '0;
        decoded = '0;
        byte_value = 0;
        select_message = 0;
        completed = 0;
        completed_raw = 0;
        completed_length = 0;
        completed_sequence = 0;
        end_sequence = {1'b0, q.sequence_number} + {49'b0, q.count};
        result = DATA;
        if (s_valid && s_ready) begin
            if (s_keep == 0 || (({1'b0, s_keep} & ({1'b0, s_keep} + 9'd1)) != 0) ||
                (!s_last && s_keep != 8'hff)) p.framing_bad = 1;
            for (integer lane = 0; lane < 8; lane = lane + 1) begin
                byte_value = s_data[lane*8 +: 8];
                if (s_keep[lane]) begin
                    if (p.bytes_seen >= MAX_PACKET_BYTES) p.framing_bad = 1;
                    else p.bytes_seen = p.bytes_seen + 1;
                    if (!p.framing_bad) begin
                        case (p.phase)
                            HEADER: begin
                                if (p.header_at < 10) p.session_id = {p.session_id[71:0], byte_value};
                                else if (p.header_at < 18) p.sequence_number = {p.sequence_number[55:0], byte_value};
                                else p.count = {p.count[7:0], byte_value};
                                if (p.header_at == 19) begin
                                    end_sequence = {1'b0, p.sequence_number} + {49'b0, p.count};
                                    if (p.count == 0 || p.count == 16'hffff) p.phase = FINISHED;
                                    else begin
                                        if (end_sequence[64]) p.framing_bad = 1;
                                        p.phase = LENGTH_HI;
                                    end
                                    p.decode_new = armed && p.session_id == expected_session &&
                                        p.sequence_number <= next_sequence && !end_sequence[64];
                                end else p.header_at = p.header_at + 1;
                            end
                            LENGTH_HI: begin
                                p.length = {byte_value, 8'b0};
                                p.phase = LENGTH_LO;
                            end
                            LENGTH_LO: begin
                                p.length[7:0] = byte_value;
                                p.body_at = 0;
                                raw_work = 0;
                                select_message = p.decode_new &&
                                    (p.sequence_number + {48'b0, p.message_index} >= next_sequence);
                                if (select_message && (p.length < 12 || p.length > 50)) p.itch_bad = 1;
                                if (p.length == 0) begin
                                    p.message_index = p.message_index + 1;
                                    p.phase = p.message_index == p.count ? FINISHED : LENGTH_HI;
                                end else p.phase = BODY;
                            end
                            BODY: begin
                                select_message = p.decode_new &&
                                    (p.sequence_number + {48'b0, p.message_index} >= next_sequence);
                                if (select_message && p.body_at < 50)
                                    raw_work[399 - p.body_at*8 -: 8] = byte_value;
                                p.body_at = p.body_at + 1;
                                if (p.body_at == p.length) begin
                                    if (select_message && !p.itch_bad) begin
                                        completed = 1;
                                        completed_raw = raw_work;
                                        completed_length = p.length;
                                        completed_sequence = p.sequence_number + {48'b0, p.message_index};
                                    end
                                    p.message_index = p.message_index + 1;
                                    p.phase = p.message_index == p.count ? FINISHED : LENGTH_HI;
                                end
                            end
                            FINISHED: p.framing_bad = 1; // Trailing bytes are never padding.
                            default: p.framing_bad = 1;
                        endcase
                    end
                end
            end
            // Minimum eligible block is 14 bytes including the Mold length, so
            // at most one completes per beat. Decode once, outside the lane loop,
            // rather than replicating the field decoder for all eight lanes.
            if (completed) begin
                decoded = decode_itch(completed_raw, completed_length);
                if (!decoded.valid) p.itch_bad = 1;
                else if (p.events == MAX_EVENTS) p.capacity_bad = 1;
                else begin
                    staged_event = decoded.data;
                    staged_event.sequence_number = completed_sequence;
                    write_event = 1;
                    write_at = EVENT_ADDR_WIDTH'(p.events);
                    p.events = p.events + 1;
                end
            end
            p.raw = raw_work;
            end_sequence = {1'b0, p.sequence_number} + {49'b0, p.count};
            // Transport faults take priority over ITCH faults. Nothing from a bad
            // datagram can become visible, even if earlier messages were valid.
            if (p.framing_bad || p.phase != FINISHED) result = MALFORMED;
            else if (!s_integrity_ok) result = INTEGRITY;
            else if (!armed) result = DISARMED;
            else if (p.session_id != expected_session) result = SESSION_MISMATCH;
            else if (p.sequence_number > next_sequence) result = GAP;
            else if (p.count == 0 || p.count == 16'hffff) begin
                if (p.sequence_number < next_sequence) result = DUPLICATE;
                else if (p.count == 0) result = HEARTBEAT;
                else result = END_SESSION;
            end else if (end_sequence[63:0] <= next_sequence) result = DUPLICATE;
            else if (p.itch_bad) result = ITCH_ERROR;
            else if (p.capacity_bad) result = CAPACITY;
        end
    end

    always @(posedge clk) begin
        status_valid <= 0;
        if (reset || arm) begin
            q <= '0; // HEADER is zero.
            draining <= 0;
            read_at <= 0;
            queued <= 0;
            armed <= !reset && arm;
            expected_session <= reset ? 80'b0 : arm_session;
            next_sequence <= reset ? 64'b0 : arm_next_sequence;
            status <= DISARMED;
        end else begin
            if (m_valid && m_ready) begin
                if (read_at + 1 == queued) draining <= 0;
                else read_at <= read_at + 1;
            end
            if (s_valid && s_ready) begin
                q <= p;
                if (write_event) events[write_at] <= staged_event;
                if (s_last) begin
                    q <= '0;
                    status_valid <= 1;
                    status <= result;
                    if (result == DATA) begin
                        next_sequence <= end_sequence[63:0];
                        queued <= p.events;
                        read_at <= 0;
                        draining <= p.events != 0;
                    end else if (result != DUPLICATE && result != HEARTBEAT) armed <= 0;
                end
            end
        end
    end
endmodule
