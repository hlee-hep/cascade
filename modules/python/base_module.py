from cascade import is_interrupted, log, log_level

class base_module:
    def __init__(self):
        self.params = {}
        self.status = "Pending"
        self.code_version_hash = ""
        self.basename = ""
        self.m_name = ""

    def check_interrupt(self):
        if is_interrupted():
            self.set_status("Interrupted")
            return True
        return False

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
        log(log_level.INFO, self.m_name, "Status : " + status)

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

    def get_code_hash(self):
        return self.code_version_hash

    def print_description(self):
        raise NotImplementedError("PythonModuleBase: print_description() must be implemented by subclass")

    def run(self):
        self.set_status("Initializing")
        self.init()
        
        if self.check_interrupt():
            return
        
        self.set_status("Running")
        self.execute()
        
        if self.check_interrupt():
            return
        
        self.set_status("Finalizing")
        self.finalize()
        
        if self.check_interrupt():
            return
        else:
            self.set_status("Done")

    def init(self):
        raise NotImplementedError("PythonModuleBase: init() must be implemented by subclass")

    def execute(self):
        raise NotImplementedError("PythonModuleBase: execute() must be implemented by subclass")
    
    def finalize(self):
        raise NotImplementedError("PythonModuleBase: finalize() must be implemented by subclass")
