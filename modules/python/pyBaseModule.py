from ROOT import logger

class pyBaseModule:
    def __init__(self):
        self.params = {}
        self.status = "Pending"
        self.code_version_hash = ""
        self.basename = ""
        self.m_name = ""
        self.log = logger.Logger.Get()

    def set_param(self, key, val):
        self.params[key] = val
    
    def set_param_from_yaml(self, path):
        import yaml
        with open(path) as f:
            data = yaml.safe_load(f)
            for k, v in data.items():
                self.set_param(k, v)

    def get_param(self, key):
        return self.params.get(key, None)

    def set_status(self, status):
        self.status = status
        self.log.Log(logger.LogLevel.INFO, self.m_name,"Status : "+status)

    def get_status(self):
        return self.status

    def set_name(self, name):
        self.m_name = name

    def name(self):
        return self.m_name

    def get_basename(self):
        return self.basename

    def get_parameters(self):
        return self.params

    def print_description(self):
        raise NotImplementedError("PythonModuleBase: print_description() must be implemented by subclass")

    def run(self):
        self.init()
        self.execute()
        self.finalize()

    def init(self):
        raise NotImplementedError("PythonModuleBase: init() must be implemented by subclass")

    def execute(self):
        raise NotImplementedError("PythonModuleBase: execute() must be implemented by subclass")
    
    def finalize(self):
        raise NotImplementedError("PythonModuleBase: finalize() must be implemented by subclass")