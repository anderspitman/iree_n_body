# IREE N-Body

Terminal n-body simulation using:

- MLIR for the simulation step kernel
- IREE for execution
- C for the host application and braille terminal rendering

The simulation kernel lives in [`n_body.mlir`](./n_body.mlir). The host app
lives in [`n_body.c`](./n_body.c).

## Runtime Model

`n_body` is a VMFB consumer. It does not compile MLIR itself.

That means there are two separate artifacts:

- `n_body`: the native executable
- `n_body.vmfb`: the compiled simulation module

The executable looks for `n_body.vmfb` in:

1. the current working directory
2. next to the `n_body` executable

This keeps the C program narrower in scope and makes distribution simpler:
someone can download just `n_body` and `n_body.vmfb` and run the simulation
without cloning the repo.

## End User Path

If you already have:

- `n_body`
- `n_body.vmfb`

you do not need the full repo.

Run the interactive simulation:

```bash
./n_body
```

Run a fixed number of steps without terminal rendering:

```bash
./n_body --headless --steps 10
```

You can also pass just a step count:

```bash
./n_body 100
```

## Developer Path

### Repo Contents

- `n_body.mlir`: MLIR step function
- `n_body.c`: host app that loads `n_body.vmfb`, runs the sim, and renders to the terminal
- `scripts/build_vmfb.sh`: generates `n_body.vmfb` from `n_body.mlir`
- `CMakeLists.txt`: builds the native app against the IREE runtime
- `third_party/iree`: vendored IREE source tree used for the runtime build

### Dependencies

Tested on Linux with:

- `python3` with `venv`
- `cmake >= 3.28`
- a C/C++ compiler toolchain

The build uses a local Python virtualenv for:

- `iree-compile`
- the Python runtime packages
- `ninja`

Notes:

- This project builds the IREE runtime from source in `third_party/iree`
- It uses the Python wheel only for `iree-compile`
- The checked-in CMake config enables IREE runtime tracing

### Initial Setup

Create the virtualenv and install the Python-side tooling:

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install ninja iree-base-compiler iree-base-runtime
```

If `third_party/iree` is not already present in your checkout, fetch the same
release used by this project:

```bash
mkdir -p third_party
git clone --depth 1 --branch v3.11.0 https://github.com/iree-org/iree.git third_party/iree
git -C third_party/iree submodule update --init --depth 1 \
  third_party/tracy \
  third_party/benchmark \
  third_party/vulkan_headers \
  third_party/printf \
  third_party/flatcc
```

### Build The Native Executable

```bash
. .venv/bin/activate
cmake -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM=$PWD/.venv/bin/ninja
cmake --build build --target n_body -j4
```

This produces:

- `build/n_body`

### Build The VMFB

Generate `n_body.vmfb` explicitly with:

```bash
./scripts/build_vmfb.sh
```

The script uses:

- `$IREE_COMPILE` if set
- otherwise `./.venv/bin/iree-compile` if present
- otherwise `iree-compile` from `PATH`

It writes:

- `n_body.vmfb`

### Developer Run

From the repo root:

```bash
./build/n_body
```

or:

```bash
./build/n_body --headless --steps 10
```

## Tracy

The runtime build enables IREE runtime tracing.

On this host, Tracy needed timer fallback enabled because invariant TSC was not
available. That is already configured in [`CMakeLists.txt`](./CMakeLists.txt).

The easiest capture path is the Python-wheel tool:

```bash
.venv/bin/iree-tracy-capture
```

Start that in one terminal, then run `./build/n_body` or `./n_body` in another.

## Important Caveat

`plan.md` asked for `n_body.c` to be C89 compliant. In practice, current IREE
headers require newer C language features, so the target is built as C11 in
[`CMakeLists.txt`](./CMakeLists.txt). The app code itself is written in a
fairly conservative C style, but strict C89 compilation is not possible without
changing the IREE dependency strategy.

## Suggested Git Ignore

These paths are generated and usually should not be committed:

```gitignore
.venv/
build/
```

Optional:

- `n_body.vmfb` can be committed if you want the compiled module checked in
- `third_party/iree` can be committed if you want a fully vendored source tree
