from cascade.pymodule import base_module
from cascade import log, log_level

class simple_module(base_module):

    def __init__(self):
        super().__init__()
        self.basename = "@BASENAME@"
        self.code_version_hash = "@VERSION_HASH@"

    def print_description(self):
        log(log_level.INFO, self.m_name, "An instance of " + self.basename + ", which is RDF based Histogram filler")

    def init(self):
        self.a = self.get_param("a") or 0
        self.b = self.get_param("b") or 0
    
    def execute(self):
        self.result = self.a + self.b
        for i in range(100_000_000):
            self.result+=i 
    
    def finalize(self):
        log(log_level.INFO, self.m_name , f"{self.a}+{self.b} = {self.result}")
