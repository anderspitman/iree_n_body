#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INPUT_MLIR="${ROOT_DIR}/n_body.mlir"
BACKEND="${BACKEND:-local-task}"
OUTPUT_VMFB=""
CUDA_TARGET="${IREE_CUDA_TARGET:-}"
VULKAN_TARGET="${IREE_VULKAN_TARGET:-}"
DEFAULT_COMPILER="${ROOT_DIR}/.venv/bin/iree-compile"

usage() {
  echo "usage: $0 [--backend local-task|local-sync|vulkan|cuda] [--output PATH] [--cuda-target TARGET] [--vulkan-target TARGET]"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend)
      BACKEND="$2"
      shift 2
      ;;
    --output|-o)
      OUTPUT_VMFB="$2"
      shift 2
      ;;
    --cuda-target)
      CUDA_TARGET="$2"
      shift 2
      ;;
    --vulkan-target)
      VULKAN_TARGET="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "${OUTPUT_VMFB}" ]]; then
  if [[ "${BACKEND}" == "local-task" ]]; then
    OUTPUT_VMFB="${ROOT_DIR}/n_body.vmfb"
  else
    OUTPUT_VMFB="${ROOT_DIR}/n_body_${BACKEND}.vmfb"
  fi
fi

if [[ -n "${IREE_COMPILE:-}" ]]; then
  COMPILER="${IREE_COMPILE}"
elif [[ -x "${DEFAULT_COMPILER}" ]]; then
  COMPILER="${DEFAULT_COMPILER}"
else
  COMPILER="iree-compile"
fi

COMPILER_ARGS=(
  "${INPUT_MLIR}"
  -o "${OUTPUT_VMFB}"
  --iree-vm-bytecode-module-strip-source-map=true
  --iree-vm-emit-polyglot-zip=false
)

case "${BACKEND}" in
  local-task|local-sync)
    COMPILER_ARGS+=(
      --iree-hal-target-device=local
      --iree-hal-local-target-device-backends=llvm-cpu
    )
    ;;
  vulkan)
    COMPILER_ARGS+=(--iree-hal-target-device=vulkan)
    if [[ -n "${VULKAN_TARGET}" ]]; then
      COMPILER_ARGS+=("--iree-vulkan-target=${VULKAN_TARGET}")
    fi
    ;;
  cuda)
    COMPILER_ARGS+=(--iree-hal-target-device=cuda)
    if [[ -n "${CUDA_TARGET}" ]]; then
      COMPILER_ARGS+=("--iree-cuda-target=${CUDA_TARGET}")
    fi
    ;;
  *)
    echo "unsupported backend: ${BACKEND}" >&2
    exit 1
    ;;
esac

"${COMPILER}" "${COMPILER_ARGS[@]}"

echo "Wrote ${OUTPUT_VMFB}"
