import cascade


ModuleStatus = cascade.ModuleStatus
ModulePhase = cascade.ModulePhase
RunResult = cascade.RunResult
CancellationToken = cascade.CancellationToken
OutputTransaction = cascade.OutputTransaction
ExecutionContext = cascade.ExecutionContext
TypedParameters = cascade.ParamManager


class base_module(cascade.IAnalysisModule):
    """Thin Python callback adapter for the C++ module lifecycle engine."""

    def __init__(self):
        super().__init__()
        self.summary = ""
        self.tags = []

    def check_interrupt(self):
        if self.is_cancellation_requested():
            self.set_status(ModuleStatus.INTERRUPTED)
            return True
        return False

    def set_param_from_yaml(self, path):
        self.load_param_from_yaml(str(path))

    def _log_component(self):
        return self.name() or self.get_basename() or self.__class__.__name__

    def log_debug(self, message):
        cascade.log(cascade.log_level.DEBUG, self._log_component(), str(message))

    def log_info(self, message):
        cascade.log(cascade.log_level.INFO, self._log_component(), str(message))

    def log_warning(self, message):
        cascade.log(cascade.log_level.WARNING, self._log_component(), str(message))

    def log_error(self, message):
        cascade.log(cascade.log_level.ERROR, self._log_component(), str(message))

    def init(self):
        raise NotImplementedError("PythonModuleBase: init() must be implemented by subclass")

    def execute(self):
        raise NotImplementedError("PythonModuleBase: execute() must be implemented by subclass")

    def finalize(self):
        raise NotImplementedError("PythonModuleBase: finalize() must be implemented by subclass")

    def on_failure(self, phase, message):
        pass

    def snapshot_state(self):
        return {}

    def get_metadata(self):
        declared = getattr(self.__class__, "METADATA", None)
        if isinstance(declared, dict):
            return {
                "name": declared.get("name", self.get_basename() or self.__class__.__name__),
                "version": declared.get("version", ""),
                "summary": declared.get("summary", ""),
                "tags": list(declared.get("tags", [])),
            }
        return {
            "name": self.get_basename() or self.__class__.__name__,
            "version": getattr(self.__class__, "VERSION", getattr(cascade, "__version__", "")),
            "summary": getattr(self.__class__, "SUMMARY", self.summary),
            "tags": list(getattr(self.__class__, "TAGS", self.tags)),
        }

    def print_description(self):
        self.log_info(getattr(self.__class__, "SUMMARY", self.summary))
