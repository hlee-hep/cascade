from cascade.pymodule import pyBaseModule
from ROOT import logger

class pySimpleModule(pyBaseModule):

    def __init__(self):
        super().__init__()
        self.basename = "@BASENAME@"
        self.code_version_hash = "@VERSION_HASH@"

    def print_description(self):
        self.log.Log(logger.LogLevel.INFO,self.m_name,"An instance of "+self.basename+", which is RDF based Histogram filler")

    def init(self):
        self.set_status("Initializing")
        self.a = self.get_param("a") or 0
        self.b = self.get_param("b") or 0
    
    def execute(self):
        self.result = self.a + self.b
        
    def finalize(self):
        self.log.Log(logger.LogLevel.INFO,self.m_name,f"{self.a}+{self.b} = {self.result}")
        self.set_status("Done")