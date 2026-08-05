import json
from pathlib import Path

from cascade.pymodule.base_module import base_module


class MassReportModule(base_module):
    VERSION = "1.0.0"
    SUMMARY = "Combines toy generation, cutflow, and resonance-fit results into a portable report."
    TAGS = ["example", "dimuon", "report", "root-free", "python"]

    def __init__(self):
        super().__init__()
        self.summary = self.SUMMARY
        self.tags = list(self.TAGS)
        self.register_param("generation", "generation.json", "Toy generator metadata")
        self.register_param("cutflow", "cutflow.json", "Selection cutflow JSON")
        self.register_param("fit_result", "fit_result.json", "Resonance fit result JSON")
        self.register_param("fit_plot", "dimuon_fit.png", "Fit plot linked from the Markdown report")
        self.register_param("output_markdown", "dimuon_report.md", "Human-readable analysis report")
        self.register_param("output_json", "analysis_summary.json", "Combined machine-readable summary")

    def init(self):
        output_paths = {
            self.final_output(self.get_param("output_markdown")),
            self.final_output(self.get_param("output_json")),
        }
        for parameter in ("generation", "cutflow", "fit_result", "fit_plot"):
            path = self.final_output(self.get_param(parameter))
            if path in output_paths:
                raise ValueError(f"{parameter} must not overwrite a report output")
            if not path.is_file():
                raise FileNotFoundError(path)
            self.track_input(path)

    @staticmethod
    def _read_json(path):
        with Path(path).open("r", encoding="utf-8") as source:
            return json.load(source)

    def execute(self):
        generation = self._read_json(self.final_output(self.get_param("generation")))
        cutflow = self._read_json(self.final_output(self.get_param("cutflow")))
        fit = self._read_json(self.final_output(self.get_param("fit_result")))
        if not fit.get("fit_valid") or fit.get("fit_status") != 0:
            raise RuntimeError(f"cannot report an invalid fit: status={fit.get('fit_status')}")

        generated_mass = float(generation["resonance_mass"])
        mass = fit["parameters"]["mass"]
        fitted_mass = float(mass["value"])
        mass_error = float(mass["error"])
        mass_pull = (fitted_mass - generated_mass) / mass_error if mass_error > 0.0 else None
        counts = cutflow["counts"]
        summary = {
            "analysis": "toy Z to dimuon resonance fit",
            "generation": generation,
            "cutflow": cutflow,
            "fit": fit,
            "comparison": {
                "generated_mass_gev": generated_mass,
                "fitted_mass_gev": fitted_mass,
                "difference_gev": fitted_mass - generated_mass,
                "mass_pull": mass_pull,
            },
        }

        with self.stage_output(self.get_param("output_json")).open("w", encoding="utf-8") as output:
            json.dump(summary, output, indent=2, sort_keys=True)
            output.write("\n")

        fit_parameters = fit["parameters"]
        width = fit_parameters["natural_width"]
        selected_efficiency = 100.0 * float(cutflow["efficiency"])
        pull_text = "n/a" if mass_pull is None else f"{mass_pull:.2f} σ"
        plot_name = Path(self.get_param("fit_plot")).name
        report = f"""# Toy Z → μ⁺μ⁻ resonance analysis

![Dimuon mass fit]({plot_name})

## Generated sample

| Quantity | Value |
| --- | ---: |
| Events | {int(generation['generated_events']):,} |
| Signal fraction | {float(generation['signal_fraction']):.3f} |
| Generated pole mass | {generated_mass:.4f} GeV |
| Generated natural width | {float(generation['resonance_width']):.4f} GeV |
| Random seed | {int(generation['random_seed'])} |

## Selection cutflow

| Selection | Events |
| --- | ---: |
| All candidates | {int(counts['all']):,} |
| Opposite sign | {int(counts['opposite_sign']):,} |
| Muon kinematics | {int(counts['kinematic']):,} |
| Histogram mass range | {int(counts['mass_window']):,} |

Overall selection efficiency: **{selected_efficiency:.2f}%**

## Resonance fit

The selected spectrum is fitted with a Voigt signal profile and an exponential continuum background.

| Fit quantity | Result |
| --- | ---: |
| Fitted mass | {fitted_mass:.4f} ± {mass_error:.4f} GeV |
| Gaussian resolution | {float(fit_parameters['resolution']['value']):.4f} ± {float(fit_parameters['resolution']['error']):.4f} GeV |
| Natural width | {float(width['value']):.4f} GeV ({'fixed' if width['fixed'] else 'floating'}) |
| Signal yield | {float(fit_parameters['signal_yield']['value']):.1f} ± {float(fit_parameters['signal_yield']['error']):.1f} |
| Background yield in fit range | {float(fit_parameters['background_yield_in_range']):.1f} |
| χ² / NDF | {float(fit['chi2_ndf']):.3f} |
| Fit status / covariance quality | {int(fit['fit_status'])} / {int(fit['covariance_quality'])} |
| Fitted − generated mass | {fitted_mass - generated_mass:+.4f} GeV ({pull_text}) |

## Reproducibility

All upstream files are tracked inputs. Cascade records their identity, the resolved parameters,
plugin artifact hashes, cache decision, and terminal result in the module and workflow provenance.
"""
        with self.stage_output(self.get_param("output_markdown")).open("w", encoding="utf-8") as output:
            output.write(report)

    def finalize(self):
        pass
