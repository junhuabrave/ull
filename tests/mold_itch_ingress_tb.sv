`timescale 1ns/1ps
module mold_itch_ingress_tb;
    import itch_pkg::*;
    logic clk = 0;
    always #5 clk = !clk;
    logic reset = 1, arm = 0;
    logic [79:0] arm_session = "TEST000001";
    logic [63:0] arm_next_sequence = 1;
    logic [63:0] s_data = 0;
    logic [7:0] s_keep = 0;
    logic s_valid = 0, s_ready, s_last = 0, s_integrity_ok = 0;
    logic m_valid, m_ready = 0, armed, status_valid;
    logic [63:0] next_sequence;
    itch_event_t m_event;
    ingress_status_t status;
    mold_itch_ingress #(.MAX_PACKET_BYTES(1024), .MAX_EVENTS(4)) dut(.*);

    integer fd, scanned, length, keep_fault, integrity, abort_output, expected_status;
    integer expected_armed, event_count, byte_value, cases = 0, checked_events = 0;
    integer aborted_events = 0, total_cases, total_events;
    bit no_stalls, saw_summary = 0;
    logic [63:0] expected_next;
    logic [7:0] bytes [0:2047];
    logic [447:0] expected_events [0:3];
    logic [447:0] held;
    string path;
    logic [7:0] op;

    task automatic tick;
        @(posedge clk); #1;
    endtask
    task automatic control(input bit do_reset);
        @(negedge clk);
        s_valid = 0; m_ready = 1;
        reset = do_reset; arm = !do_reset;
        #1;
        if (s_ready || m_valid) $fatal(1,"abort must inhibit both handshakes");
        tick();
        if (m_valid || status_valid) $fatal(1,"abort leaked staged data/status");
        if (armed !== !do_reset || next_sequence !== (do_reset ? 64'b0 : arm_next_sequence))
            $fatal(1,"abort/checkpoint mismatch");
        @(negedge clk); reset = 0; arm = 0; m_ready = 0;
    endtask

    task automatic send_packet;
        integer at, count;
        begin
            // At least one beat represents an empty (invalid) payload.
            at = 0;
            m_ready = no_stalls && abort_output == 0;
            do begin
                @(negedge clk);
                s_valid = 0;
                // Bubbles carry poison bytes; a parser must ignore invalid beats.
                s_data = '1; s_keep = 8'h55; s_last = 1; s_integrity_ok = 0;
                if (!no_stalls && (cases + at) % 3 == 0) begin
                    tick();
                    if (m_valid || status_valid) $fatal(1,"case %0d: event/status before EOF",cases);
                    @(negedge clk);
                end
                s_data = 0; s_keep = 0;
                count = length - at; if (count > 8) count = 8;
                for (integer lane = 0; lane < count; lane = lane + 1) begin
                    s_data[lane*8 +: 8] = bytes[at + lane]; s_keep[lane] = 1;
                end
                s_last = at + count == length;
                s_integrity_ok = s_last && integrity != 0; // Only EOF integrity is meaningful.
                if (s_last && keep_fault == 1) s_keep = 0;
                if (s_last && keep_fault == 2) s_keep = 8'h55;
                if (at == 0 && keep_fault == 3) s_keep = 8'h7f;
                s_valid = 1;
                #1;
                if (!s_ready || m_valid) $fatal(1,"case %0d: unexpected input stall/early output",cases);
                tick();
                if (!s_last && (m_valid || status_valid)) $fatal(1,"case %0d: event/status before EOF",cases);
                at = at + count;
            end while (at < length);
            if (!status_valid || status !== expected_status[3:0] || next_sequence !== expected_next || armed !== expected_armed[0])
                $fatal(1,"case %0d: status %0d/%0d next %h/%h armed %b/%b",cases,
                    status,expected_status,next_sequence,expected_next,armed,expected_armed[0]);
            if (m_valid !== (event_count != 0)) $fatal(1,"case %0d: incorrect visibility at EOF",cases);
            @(negedge clk); s_valid = 0;

            if (abort_output != 0) begin
                if (!m_valid) $fatal(1,"abort test requires staged output");
                aborted_events = aborted_events + event_count;
                arm_session = "TEST000001"; arm_next_sequence = 7;
                control(abort_output == 1);
            end else begin
                for (integer i = 0; i < event_count; i = i + 1) begin
                    held = m_event;
                    if (!m_valid || held !== expected_events[i])
                        $fatal(1,"case %0d event %0d: got %h expected %h",cases,i,m_event,expected_events[i]);
                    repeat (no_stalls ? 0 : 1 + (cases + i) % 4) begin
                        tick();
                        if (!m_valid || s_ready || m_event !== held || status_valid)
                            $fatal(1,"case %0d: stalled output unstable or status repeated",cases);
                        @(negedge clk);
                    end
                    m_ready = 1; tick();
                    @(negedge clk); m_ready = no_stalls;
                    checked_events = checked_events + 1;
                end
                tick();
                if (m_valid || !s_ready || status_valid) $fatal(1,"case %0d: trailing event/stall/status",cases);
            end
            cases = cases + 1;
        end
    endtask

    initial begin
        no_stalls = $test$plusargs("no_stalls");
        if (!$value$plusargs("vectors=%s",path)) $fatal(1,"missing +vectors=path");
        fd = $fopen(path,"r"); if (!fd) $fatal(1,"cannot open vectors");
        tick(); @(negedge clk); reset = 0;
        while (!$feof(fd)) begin
            scanned = $fscanf(fd,"%s",op);
            if (scanned == 1) begin
                case (op)
                    "A": begin
                        if ($fscanf(fd,"%h %h",arm_session,arm_next_sequence) != 2) $fatal(1,"bad arm vector");
                        control(0);
                    end
                    "R": control(1);
                    "C": begin
                        @(negedge clk); s_valid = 1; s_keep = 8'hff;
                        s_data = 64'h3030303054534554; s_last = 0;
                        tick(); if (m_valid || status_valid) $fatal(1,"partial header leaked");
                        @(negedge clk); s_valid = 0;
                    end
                    "P": begin
                        if ($fscanf(fd,"%d %d %d %d %d %h %d %d",length,keep_fault,integrity,
                            abort_output,expected_status,expected_next,expected_armed,event_count) != 8)
                            $fatal(1,"bad packet vector");
                        if (length > 2048 || event_count > 4) $fatal(1,"fixture exceeds testbench bounds");
                        for (integer i = 0; i < length; i = i + 1) begin
                            if ($fscanf(fd,"%h",byte_value) != 1) $fatal(1,"missing byte");
                            bytes[i] = byte_value[7:0];
                        end
                        for (integer i = 0; i < event_count; i = i + 1)
                            if ($fscanf(fd,"%h",expected_events[i]) != 1) $fatal(1,"missing expected event");
                        send_packet();
                    end
                    "Z": begin
                        if ($fscanf(fd,"%d %d",total_cases,total_events) != 2) $fatal(1,"bad totals");
                        if (cases != total_cases || checked_events + aborted_events != total_events)
                            $fatal(1,"missing packet/event checks");
                        saw_summary = 1;
                    end
                    default: $fatal(1,"unknown vector command %s",op);
                endcase
            end
        end
        $fclose(fd);
        if (!saw_summary || cases == 0) $fatal(1,"incomplete corpus");
        $display("PASS: %0d RTL ingress packets, %0d C++-matched events; no_stalls=%0d, EOF integrity, reset/rearm",cases,checked_events,no_stalls);
        $finish;
    end
    initial begin
        #100000000;
        $fatal(1,"simulation watchdog expired");
    end
endmodule
