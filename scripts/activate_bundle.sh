#!/usr/bin/env bash

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "Source this file instead of executing it: source $0" >&2
    exit 2
fi

_cascade_bundle_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
_cascade_venv_activate="${_cascade_bundle_dir}/.venv/bin/activate"
_cascade_root_dir="${_cascade_bundle_dir}/root"

if [[ ! -f "${_cascade_venv_activate}" ]]; then
    echo "Cascade bundle Python environment is missing: ${_cascade_venv_activate}" >&2
    unset _cascade_bundle_dir _cascade_venv_activate _cascade_root_dir
    return 1
fi
if [[ ! -x "${_cascade_root_dir}/bin/root-config" ]]; then
    echo "Cascade bundle ROOT environment is missing: ${_cascade_root_dir}" >&2
    unset _cascade_bundle_dir _cascade_venv_activate _cascade_root_dir
    return 1
fi

source "${_cascade_venv_activate}"

export CASCADE_PREFIX="${_cascade_bundle_dir}"
export CASCADE_CONFIG_FILE="${_cascade_bundle_dir}/var/cascade/config.json"
export CASCADE_PYTHON_RUNTIME_DIR="${_cascade_bundle_dir}/lib"
export ROOTSYS="${_cascade_root_dir}"
export PATH="${_cascade_bundle_dir}/bin:${ROOTSYS}/bin:${PATH}"
export PYTHONPATH="${_cascade_bundle_dir}/lib:${ROOTSYS}/lib${PYTHONPATH:+:${PYTHONPATH}}"
export LD_LIBRARY_PATH="${_cascade_bundle_dir}/lib:${ROOTSYS}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${ROOTSYS}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

unset _cascade_bundle_dir _cascade_venv_activate _cascade_root_dir
