`timescale 1ns / 1ps
`define intN 32
`define addrN 9
`include "primitives.v"
`include "tests_stream_compute_fn.v"
`include "array.v"

module tests_stream_compute_fn_tb;

    localparam N = 256;

    `wire(Array, (`addrN, `intN), arr);
    wire `intT res;
    integer write_seed = 21;
    integer read_seed = 42;

    `reg(stream, `intN, sRA);
    `wire(stream, `intN, sR);
    `reg(stream, `intN, sWA);
    `reg(stream, `intN, sW);
    `wire(stream, 1, sB);
    `assign_stream(inst, sRA);
    `assign_stream(inst, sWA);
    `assign_stream(inst, sW);

    assign sR_ready = out_ready;
    assign sB_ready = out_ready;

    integer counter = 0;

    `testbench(tests_stream_compute_fn_tb, 30000)

    // Use bit 31 to indicate that data is invalid
    array #(.N(2 * N), .INIT_ADDR(0), .INIT(1 << 31))
      arr(.clk(clk),
          `out(Array, 0, arr));

    `in_ready(inst);
    `inst_sync(tests_stream_compute_fn, inst, #())(
      `sync(in_valid, out_ready),
      `in(Array, 0, arr),
      `in(stream, 1, sRA),
      `in(stream, 2, sWA),
      `in(stream, 3, sW),
      `out(stream, 0, sR),
      `out(null_stream, 1, sB));

    integer i;
    integer start_time = 0;
    integer max_time   = 0;
    integer write_time = 0;
    integer max_addr   = 0;
    initial begin
        sRA  = N;
        sRA_valid = `false;
        sWA  = 0;
        sWA_valid = `false;
        sW = 0;
        sW_valid = `false;
        `start;

        // write some data
        for(i = 0; i < N; i = i + 1) begin
            start_time = counter;
            sWA        = i;
            sWA_valid  = `true;
            sW         = i;
            sW_valid   = `true;
            `wait_for(sWA_ready && sW_ready);
            write_time = counter - start_time;
            if(write_time > max_time) begin
                max_time = write_time;
                max_addr = i;
            end
            if($random(write_seed) & 1) begin
                sWA_valid = `false;
                sW_valid = `false;
                @(posedge clk);
                sWA_valid = `true;
                sW_valid = `true;
            end
        end
        sWA_valid = `false;
        sW_valid = `false;
        `wait_for(sRA >= N + N-1 && sR_valid);
        nrst = `false;
        #2;
        $display("done @ %d", counter);
        $display("max write time = %d @ %d", max_time, max_addr);
        $finish;
    end

always @(posedge clk) begin
    counter <= counter + 1;
    if(sRA_ready) begin
        sRA_valid <= $random(read_seed);
    end
    if(sR_valid & (!sR[31])) begin
        $display("arr(%d) = %d", sRA, sR);
        sRA <= sRA + 1;
    end
end

endmodule // tests_stream_compute_fn_tb