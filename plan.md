Create an n-body simulation using MLIR, C, and IREE.

The simulation step function should live in n_body.mlir.

The C code should live in n_body.c. It should be C89 compliant. This program
will compile the MLIR to n_body.vmfb, and then load it and run the sim for n
steps.

The sim will be visualized in the terminal, using the braille rendering
techniques from `fall.c`. Port the necessary code into n_body.c; don't use
fall.c directly.

Download whatever iree stuff is necessary.

I also want it to have tracy support. If there are pre-compiled iree libs that
have this enabled, great. Otherwise compile iree with tracy support.

Let me know if anything is unclear and ask me before you start.
