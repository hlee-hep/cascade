from typing import Any

class RuntimeOptions:
    input_hash: str
    output_hash: str
    dag_workers: int
    isolated_timeout_seconds: float
    progress_interval_ms: int
    artifact_hash_cache_entries: int

from .py_amcm import Controller as Controller
from .py_amcm import WorkflowProvenanceError as WorkflowProvenanceError

def configure_runtime(**values: Any) -> RuntimeOptions: ...
def get_runtime_options() -> RuntimeOptions: ...
def set_runtime_options(options: RuntimeOptions) -> None: ...
def get_version() -> str: ...
def get_abi_version() -> int: ...
def get_abi_tag() -> str: ...
def __getattr__(name: str) -> Any: ...

__version__: str
__abi_version__: int
__abi_tag__: str

from .plot_data import histogram_panel as histogram_panel

from .publication import PublicationFigure as PublicationFigure

from .publication import PublicationLayout as PublicationLayout

from .publication import PublicationStyle as PublicationStyle
