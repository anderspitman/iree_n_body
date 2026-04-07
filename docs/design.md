# Design

## Goals

The design was driven by a few concrete goals from the project brief:

- put the simulation step in MLIR
- use IREE to execute the compiled kernel
- keep the host side in C
- render the simulation in a terminal using braille techniques derived from
  `fall.c`
- support Tracy-enabled runtime tracing

The current design meets those goals while keeping the runtime binary narrowly
focused on execution rather than compilation.

## Final Structure

The final implementation is intentionally split into:

- a compiled simulation module:
  [`n_body.mlir`](/mnt/share/code/iree_n_body/n_body.mlir) ->
  `n_body.vmfb`
- a runtime executable:
  [`n_body.c`](/mnt/share/code/iree_n_body/n_body.c)
- a small build script for the VMFB:
  [`scripts/build_vmfb.sh`](/mnt/share/code/iree_n_body/scripts/build_vmfb.sh)

This is a better fit than putting MLIR compilation inside the host executable.

## Why The C Program No Longer Compiles MLIR

The first implementation path embedded MLIR compilation behavior into the host
application. That worked, but it broadened the program’s scope in ways that
were not actually useful at runtime:

- the executable had to know where the source tree lived
- the executable had to know how to find `iree-compile`
- end-user and developer concerns were mixed together
- failure modes became less clear because runtime loading and build-time
  compilation were coupled

Moving compilation into `scripts/build_vmfb.sh` improves the design:

- `n_body` becomes a plain VMFB consumer
- the binary is easier to distribute with a prebuilt `n_body.vmfb`
- the runtime code is smaller and more focused
- the developer workflow is still straightforward

This also aligns better with how IREE is commonly used in production settings:
compilation is usually a build or packaging step, not something the deployed
runtime binary performs on the fly.

## Why The Project Still Uses Python

The project still uses Python for `iree-compile`, even though the runtime is
built from vendored IREE source.

That choice was a tradeoff:

- building the runtime from `third_party/iree` keeps the native execution path
  self-contained and controllable
- using the Python-provided `iree-compile` avoids pulling the full compiler
  build into the local native build graph

Building the runtime is already manageable. Building the compiler stack from
source would be substantially heavier because it brings in LLVM/MLIR and
related dependencies. For this demo, the lighter approach is more pragmatic.

The downside is split provenance:

- runtime from vendored source
- compiler from Python tooling

That is mitigated by pinning the IREE source checkout to `v3.11.0` and using a
matching release line for the Python compiler package. It is not ideal, but it
is a reasonable compromise for a small project.

## Simulation Design

### State Layout

Each body is represented by five floats:

- `x`
- `y`
- `vx`
- `vy`
- `mass`

The host and MLIR kernel both use this same packed layout. That keeps the
interface between the runtime and the kernel simple:

- one tensor input for body state
- three scalar inputs for constants
- one tensor output for the next state

This is easier to reason about than splitting positions, velocities, and masses
across multiple tensors.

### Fixed Body Count

The kernel currently uses a static tensor shape of `tensor<8x5xf32>`.

That was not the first shape attempted. An earlier dynamic-shape form worked at
the MLIR compilation level, but led to a runtime issue involving
`hal.device.queue.alloca` on this configuration. Since the host was already
using a fixed body count, switching the kernel to a fixed shape was the simpler
and more reliable design.

This choice improves:

- predictability
- runtime simplicity
- compatibility with the current local CPU execution path

The downside is reduced flexibility. Supporting arbitrary body counts would be
a reasonable future improvement, but it should be done deliberately and tested
against the chosen IREE backend.

### Numerical Model

The kernel computes all pairwise interactions directly:

- for each body `i`
- iterate over every body `j`
- skip `i == j`
- compute softened inverse-distance acceleration
- integrate velocity and position using a simple explicit step

This is an `O(n^2)` step, which is appropriate here because the simulation size
is intentionally small.

The integration model is simple rather than physically sophisticated. That was
intentional:

- the goal is a compact, visible terminal simulation
- not a high-accuracy astrophysics solver

Potential improvements include:

- symplectic integration
- adaptive time steps
- energy diagnostics
- collision or boundary handling

## Host Runtime Design

### Runtime Responsibilities

The host program is responsible for:

- locating `n_body.vmfb`
- building an IREE runtime instance
- creating a `local-task` device
- loading the VMFB into a session
- marshalling inputs and outputs for `module.step`
- rendering the state in the terminal

It intentionally does not:

- compile MLIR
- mutate the VMFB
- depend on repository-only paths for normal execution

### VMFB Lookup Strategy

The executable looks for `n_body.vmfb` in:

1. the current working directory
2. the directory containing the executable

This lookup order was chosen to support two practical workflows:

- developer runs from the repo root
- end-user runs from a bundle directory containing only `n_body` and
  `n_body.vmfb`

That is a better deployment model than assuming the source tree is present.

### Call ABI Choices

The runtime call uses:

- a buffer view for the body-state tensor
- VM primitive values for the scalar arguments

That detail matters. An earlier approach passed the scalars as rank-0 buffer
views, which did not match the function ABI and failed at runtime. Using
primitive VM values for scalar arguments is the correct design for this kernel
signature.

### Output Handling

The kernel returns a tensor, and the host copies it back to CPU memory with
`iree_hal_device_transfer_d2h`.

That is simple and appropriate here because:

- state is small
- rendering is CPU-side
- the host needs immediate access to the results each frame

If the project grew into a much larger simulation, reducing host-device copies
or reusing allocations more aggressively would be worth considering.

## Terminal Rendering Design

The renderer is based on the braille approach from
[`fall.c`](/mnt/share/code/iree_n_body/fall.c), but the required code was
ported into [`n_body.c`](/mnt/share/code/iree_n_body/n_body.c) instead of
reusing `fall.c` directly.

That decision follows the original requirement and also keeps the final
application self-contained.

The rendering system includes:

- terminal size detection
- subcell mapping into braille space
- braille dot accumulation
- line drawing for trails
- small circles for body markers

Trails are stored in ring buffers per body. That makes motion readable without
turning the display into full-frame persistence noise.

Potential rendering improvements:

- distinct glyph sizing by mass
- velocity vectors
- color output
- camera framing or zoom controls
- trail fading instead of fixed trail depth

## Build System Design

### Native Build

[`CMakeLists.txt`](/mnt/share/code/iree_n_body/CMakeLists.txt) builds the host
program against the IREE runtime from `third_party/iree`.

Important build choices:

- `IREE_BUILD_COMPILER=OFF`
- `IREE_BUILD_TESTS=OFF`
- `IREE_BUILD_SAMPLES=OFF`
- runtime tracing enabled
- Tracy capture tool build disabled

This keeps the native build smaller and focused on what the app actually needs.

### Why Tracy Capture Is Not Built Natively

The project enables runtime tracing, but does not build the separate Tracy
capture binary from the vendored IREE tree.

That was a practical choice:

- building the Tracy capture tool required extra system dependencies
- the project itself only needs runtime tracing in the executable
- the standalone capture utility can come from the Python tooling path when
  needed

### Tracy Timer Fallback

Runtime tracing originally failed on this host because Tracy rejected the CPU’s
timer mode due to missing invariant TSC support. Enabling `TRACY_TIMER_FALLBACK`
in the native build resolved that issue.

That is a platform compatibility choice rather than a design ideal; lower
resolution timing is an acceptable tradeoff here because the project benefits
more from “tracing works” than from squeezing maximum timer fidelity out of a
demo.

## C Language Level

The initial brief asked for C89 compliance.

In practice, current IREE headers require newer language features, including
constructs that are not compatible with strict C89. Because of that, the target
is built as C11.

The important point is that this is imposed by the dependency boundary, not by
the application logic. The host code itself is still fairly conservative in
style.

If strict C89 were a hard requirement, the design would need to change more
radically, for example:

- wrapping IREE behind a separate compatibility layer
- moving the IREE-facing code into a C++ or newer-C translation unit
- or choosing a different runtime strategy

## Pitfalls And Known Weaknesses

### Version Coordination

The runtime and compiler come from different distribution paths:

- runtime: vendored IREE source
- compiler: Python package

Even when release versions are matched, this requires discipline.

### Static Shape

The simulation is fixed at eight bodies. That simplifies the runtime path, but
it also hardcodes a limitation into both host and kernel.

### Performance Is Fine, But Not Optimized

The design is intentionally straightforward:

- allocate input buffer view each step
- invoke
- copy output back
- update trails
- render

That is perfectly acceptable for a small terminal simulation, but not a design
to copy into a large-scale high-frequency compute application.

### Limited Error UX

Failures are surfaced through stderr and IREE status reporting. That is enough
for developers, but packaging this for broader end users would benefit from
clearer guidance when:

- `n_body.vmfb` is missing
- the runtime cannot load the module
- tracing tooling is unavailable

## Potential Improvements

### Functional Improvements

- support variable body counts
- add CLI options for gravity, time step, scale, and trail length
- offer deterministic presets
- add pause/reset controls

### Runtime Improvements

- reuse device allocations across steps
- reuse the call object instead of rebuilding it per step
- explore static module embedding as an alternative distribution model

### Build And Packaging Improvements

- update README setup to remove now-unneeded `iree-base-runtime`
- add `.gitignore`
- add release packaging that emits:
  - `n_body`
  - `n_body.vmfb`
  - a short runtime README

### Documentation Improvements

- document the exact MLIR function ABI
- document the expected IREE release version more explicitly
- add a troubleshooting document for missing dependencies and tracing issues

## Summary

The current design is intentionally pragmatic:

- keep simulation logic in MLIR
- keep runtime logic in C
- keep compilation outside the runtime binary
- keep the deployment model simple enough to distribute as `n_body` plus
  `n_body.vmfb`

It is not the most general design, but it is coherent, easy to operate, and
well matched to the scope of the project. The main limitations are known and
localized, which makes future iteration straightforward.
