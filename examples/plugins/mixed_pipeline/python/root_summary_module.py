import json

from cascade.pymodule.base_module import base_module


class RootSummaryModule(base_module):
    SUMMARY = "Summarizes a C++-generated ROOT TTree with PyROOT."
    TAGS = ["example", "root", "mixed-language", "transaction"]

    def __init__(self):
        super().__init__()
        self.basename = "@BASENAME@"
        self.code_version_hash = "@VERSION_HASH@"
        self.summary = self.SUMMARY
        self.tags = list(self.TAGS)
        self.register_param("input", "events.root", "ROOT input path relative to the output directory")
        self.register_param("manifest", "events_manifest.json", "C++-generated portable metadata")
        self.register_param("output", "events_summary.json", "Transactional JSON output path")

    def print_description(self):
        print(self.SUMMARY)

    def init(self):
        self.track_input(self.final_output(self.get_param("input")))
        self.track_input(self.final_output(self.get_param("manifest")))

    def execute(self):
        root_path = self.final_output(self.get_param("input"))
        if not root_path.exists():
            raise RuntimeError("ROOT input is missing")
        try:
            import ROOT
        except ImportError:
            with self.final_output(self.get_param("manifest")).open("r", encoding="utf-8") as source:
                summary = json.load(source)
            summary["backend"] = "manifest-fallback"
        else:
            input_file = ROOT.TFile.Open(str(root_path), "READ")
            if not input_file or input_file.IsZombie():
                raise RuntimeError("cannot open ROOT input")
            tree = input_file.Get("events")
            if not tree:
                input_file.Close()
                raise RuntimeError("events tree is missing")

            values = [float(event.value) for event in tree]
            summary = {
                "entries": int(tree.GetEntries()),
                "minimum": min(values) if values else None,
                "maximum": max(values) if values else None,
                "mean": sum(values) / len(values) if values else None,
                "backend": "PyROOT",
            }
            input_file.Close()
        with self.stage_output(self.get_param("output")).open("w", encoding="utf-8") as output:
            json.dump(summary, output, indent=2)
            output.write("\n")

    def finalize(self):
        pass
