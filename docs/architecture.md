# Architecture

## Overview

The project is split into two independent artifacts:

- `n_body.vmfb`: the compiled simulation kernel
- `n_body`: the native host executable

The VMFB is generated from [`n_body.mlir`](/mnt/share/code/iree_n_body/n_body.mlir)
using `iree-compile`. The native executable is built from
[`n_body.c`](/mnt/share/code/iree_n_body/n_body.c) and links against the IREE
runtime built from the vendored [`third_party/iree`](/mnt/share/code/iree_n_body/third_party/iree)
source tree.

## Main Components

### Simulation Kernel

[`n_body.mlir`](/mnt/share/code/iree_n_body/n_body.mlir) exports a single
`@step` function:

- input: `tensor<8x5xf32>` plus scalar simulation constants
- output: `tensor<8x5xf32>`

Each body uses five floats:

- `x`
- `y`
- `vx`
- `vy`
- `mass`

The kernel computes all pairwise accelerations and returns the next simulation
state.

### Host Runtime

[`n_body.c`](/mnt/share/code/iree_n_body/n_body.c) is responsible for:

- locating `n_body.vmfb`
- creating the IREE runtime instance and session
- loading the bytecode module
- packing and unpacking body state
- invoking `module.step`
- rendering the result in the terminal

It does not compile MLIR. VMFB generation is explicitly handled outside the C
program.

### VMFB Build Script

[`scripts/build_vmfb.sh`](/mnt/share/code/iree_n_body/scripts/build_vmfb.sh)
compiles `n_body.mlir` into `n_body.vmfb`.

This keeps compiler concerns out of the runtime executable and allows the
binary to be distributed together with a prebuilt VMFB.

### Build System

[`CMakeLists.txt`](/mnt/share/code/iree_n_body/CMakeLists.txt) builds the host
executable and configures IREE as a subproject:

- runtime only
- compiler disabled
- runtime tracing enabled
- Tracy capture binary disabled
- Tracy timer fallback enabled

## Data Flow

1. `scripts/build_vmfb.sh` compiles `n_body.mlir` into `n_body.vmfb`
2. `n_body` starts and locates `n_body.vmfb`
3. the host creates an IREE runtime instance and `local-task` device
4. the VMFB is loaded into a runtime session
5. body state is packed into a contiguous `8x5` float buffer
6. the host invokes `module.step`
7. the output tensor is copied back to host memory
8. the host updates trails and renders a braille frame

## Runtime Lookup Model

The executable looks for `n_body.vmfb` in:

1. the current working directory
2. the executable directory

That supports both:

- developer runs from the repo root
- standalone distribution of `n_body` plus `n_body.vmfb`

## Tracing Model

IREE runtime tracing is enabled in the native runtime build. Tracy is linked
into the runtime-enabled executable, and captures can be collected with the
separate `iree-tracy-capture` tool when available.

## Constraints

- Body count is fixed at `8`
- Simulation uses `f32` throughout
- The host builds as C11 because current IREE headers require newer language
  features than C89
