import json

from cascade.pymodule.base_module import base_module


class TextTransformModule(base_module):
    SUMMARY = "Reads C++ JSON output and writes a Python transformation."
    TAGS = ["example", "mixed-language", "transaction"]

    def __init__(self):
        super().__init__()
        self.basename = "@BASENAME@"
        self.code_version_hash = "@VERSION_HASH@"
        self.summary = self.SUMMARY
        self.tags = list(self.TAGS)
        self.register_param("input", "message.json", "Input path relative to the output directory")
        self.register_param("output", "message_upper.json", "Transactional output path")

    def print_description(self):
        print(self.SUMMARY)

    def init(self):
        if self.final_output(self.get_param("input")) == self.final_output(self.get_param("output")):
            raise ValueError("input and output must differ")
        self.track_input(self.final_output(self.get_param("input")))

    def execute(self):
        with self.final_output(self.get_param("input")).open("r", encoding="utf-8") as source:
            payload = json.load(source)
        transformed = {
            "messages": [str(message).upper() for message in payload["messages"]],
            "producer": "TextTransformModule",
        }
        with self.stage_output(self.get_param("output")).open("w", encoding="utf-8") as output:
            json.dump(transformed, output, indent=2)
            output.write("\n")

    def finalize(self):
        pass
