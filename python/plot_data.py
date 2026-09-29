"""Prepare NumPy event arrays for the shared publication panel schema.

This adapter owns histogramming and symmetric sumw2 errors, never rendering.
ROOT exports retain their own error model; the renderer consumes either source.
"""


def histogram_panel(data=None, *, backgrounds=(), bins=50, hist_range=None,
                    weights=None, background_weights=None, labels=None, colors=None,
                    data_label="Data", data_color="black", data_draw="PE",
                    ratio=False, ratio_ylim=(0.5, 1.5), density=False,
                    xlabel="X", ylabel="Events", log_y=False, sample=None):
    """Return a JSON-compatible panel for ``PublicationFigure.add``.

    Each background is one event array. All distributions share bin edges, inferred
    from their combined range when omitted. Weights must match their event arrays.
    Unweighted errors are sqrt(N); weighted errors are sqrt(sumw2). Ratio points
    carry numerator errors; the separate band carries denominator uncertainty.
    Nonpositive denominator bins are omitted, matching the former NumPy helper.
    Density normalizes data and the total background separately to unit area.
    """
    import numpy as np

    def array(value):
        result = np.asarray(value, dtype=float)
        if result.ndim != 1 or not np.isfinite(result).all():
            raise ValueError("Events and weights must be finite one-dimensional arrays")
        return result

    backgrounds = [array(values) for values in backgrounds]
    data = None if data is None else array(data)
    if data is None and not backgrounds:
        raise ValueError("Provide data or at least one background")
    if data is None and weights is not None:
        raise ValueError("Data weights require data")
    if ratio and (data is None or not backgrounds):
        raise ValueError("Ratio requires data and backgrounds")
    count = len(backgrounds)
    background_weights = [None]*count if background_weights is None else list(background_weights)
    labels = [f"Background {i+1}" for i in range(count)] if labels is None else list(labels)
    palette = ("#4477AA", "#EE6677", "#228833", "#CCBB44", "#66CCEE")
    colors = [palette[i % len(palette)] for i in range(count)] if colors is None else list(colors)
    if any(len(values) != count for values in (background_weights, labels, colors)):
        raise ValueError("Background weights, labels, and colors must match backgrounds")
    arrays = backgrounds + ([] if data is None else [data])
    edges = np.histogram_bin_edges(np.concatenate(arrays), bins=bins, range=hist_range)
    if len(edges) < 2 or not np.isfinite(edges).all() or not (np.diff(edges) > 0).all():
        raise ValueError("Histogram edges must be finite and strictly increasing")

    def histogram(values, weight, label, color, draw, is_data):
        weight = None if weight is None else array(weight)
        if weight is not None and weight.shape != values.shape:
            raise ValueError("Weights must match their event array")
        sums = np.histogram(values, bins=edges, weights=weight)[0].astype(float)
        squares = sums if weight is None else np.histogram(values, bins=edges, weights=weight*weight)[0]
        errors = np.sqrt(squares)
        if not np.isfinite(sums).all() or not np.isfinite(errors).all():
            raise ValueError("Histogram contents and errors must be finite")
        return dict(kind="hist", edges=edges.tolist(), values=sums.tolist(),
                    error_low=errors.tolist(), error_high=errors.tolist(),
                    label=label, color=dict(line=color, fill=color, marker=color),
                    draw=draw, is_data=is_data, zero_error=True, legend=bool(label))

    stacks = [histogram(values, weight, label, color, "HIST", False)
              for values, weight, label, color in zip(backgrounds, background_weights, labels, colors)]
    overlays = [] if data is None else [histogram(data, weights, data_label, data_color, data_draw, data_draw != "HIST")]
    if density:
        for group in (stacks, overlays):
            if not group:
                continue
            total = sum(sum(item["values"]) for item in group)
            if total <= 0:
                raise ValueError("Density normalization requires a positive total weight")
            scale = 1/(total*np.diff(edges))
            for item in group:
                for key in ("values", "error_low", "error_high"):
                    item[key] = (np.asarray(item[key])*scale).tolist()
    panel = dict(schema_version=1, stacks=stacks, overlays=overlays,
                 xlabel=xlabel, ylabel=ylabel, log_y=bool(log_y),
                 legend_enable=True, legend_columns=1, ymin=None, ymax=None,
                 cut_lower=None, cut_upper=None,
                 sample=dict(enabled=False, experiment="", comment="", luminosity=0,
                             luminosity_unit="fb^{-1}"))
    if sample is not None:
        panel["sample"].update(sample)
    centers, widths = (edges[1:]+edges[:-1])/2, np.diff(edges)/2

    def graph(x, y, xerror, error):
        return dict(kind="graph", x=x.tolist(), y=y.tolist(),
                    xerror_low=xerror.tolist(), xerror_high=xerror.tolist(),
                    error_low=error.tolist(), error_high=error.tolist())

    if stacks:
        total = np.sum([item["values"] for item in stacks], axis=0)
        error = np.sqrt(np.sum(np.square([item["error_low"] for item in stacks]), axis=0))
        panel["band"] = dict(graph(centers, total, widths, error),
                             edges=edges.tolist(), label="MC stat")
    if ratio:
        valid = total > 0
        numerator = np.asarray(overlays[0]["values"])
        numerator_error = np.asarray(overlays[0]["error_low"])
        panel["ratio"] = dict(
            points=graph(centers[valid], numerator[valid]/total[valid],
                         np.zeros(valid.sum()), numerator_error[valid]/total[valid]),
            band=graph(centers[valid], np.ones(valid.sum()), widths[valid], error[valid]/total[valid]),
            ylabel="Data/MC", ymin=float(ratio_ylim[0]), ymax=float(ratio_ylim[1]),
            split=0.3, unity_line=True, arrows=False, error_mode="symmetric")
    from .publication import _validate
    _validate(panel)
    return panel
