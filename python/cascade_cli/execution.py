import os
from typing import Any, Dict, List, Optional, Tuple

from .common import (
    _apply_parameters,
    _emit,
    _load_controller,
    _load_mapping,
    _parse_kv,
    _redirect_stdout_to_stderr,
    _resolve_config_path,
    _result_payload,
    _split_parameter_ref,
    _validate_keys,
)


def cmd_module_list(args) -> None:
    with _redirect_stdout_to_stderr(args.json):
        controller = _load_controller(args.json, getattr(args, "require_signed", False))
        modules = controller.get_list_available_module_metadata()
    if args.language:
        modules = [item for item in modules if item["language"] == args.language]
    if args.tag:
        modules = [item for item in modules if args.tag in item["tags"]]
    modules.sort(key=lambda item: (item["name"], item["language"]))
    if args.json:
        _emit({"modules": modules}, True)
        return
    if not modules:
        print("No modules found.")
        return
    for item in modules:
        version = f" {item['version']}" if item["version"] else ""
        summary = f" - {item['summary']}" if item["summary"] else ""
        tags = f" [{', '.join(item['tags'])}]" if item["tags"] else ""
        print(f"{item['name']} ({item['language']}){version}{tags}{summary}")


def cmd_module_run(args) -> None:
    with _redirect_stdout_to_stderr(args.json):
        controller = _load_controller(args.json, getattr(args, "require_signed", False))
        handle = controller.register_module(args.module, args.name)
        if args.output_directory:
            handle.set_output_directory(os.path.abspath(args.output_directory))
        if args.cache_directory:
            handle.set_cache_directory(os.path.abspath(args.cache_directory))
        if args.params:
            _apply_parameters(handle, _load_mapping(args.params))
        _apply_parameters(handle, dict(_parse_kv(value) for value in args.set))
        result = controller.run_module(handle.name(), isolated=args.isolated)
    payload = _result_payload(result, handle.name())
    payload["run_id"] = handle.get_run_id()
    payload["provenance"] = handle.get_last_provenance_path()
    if args.json:
        _emit(payload, True)
    else:
        detail = f": {payload['message']}" if payload["message"] else ""
        print(f"{payload['name']}: {payload['status']} ({payload['phase']}){detail}")
    if not payload["allows_dependents"]:
        raise SystemExit(1)


def _validate_dag_structure(modules, links) -> None:
    names = {item["name"] for item in modules}
    dependencies = {item["name"]: item["dependencies"] for item in modules}
    for name, required in dependencies.items():
        if len(required) != len(set(required)):
            raise ValueError(f"duplicate dependency for DAG module: {name}")
        if name in required:
            raise ValueError(f"DAG module cannot depend on itself: {name}")
        missing = sorted(set(required) - names)
        if missing:
            raise ValueError(f"DAG module '{name}' depends on missing module(s): {', '.join(missing)}")

    state = {}
    stack = []

    def visit(name):
        if state.get(name) == "done":
            return
        if state.get(name) == "visiting":
            start = stack.index(name)
            raise ValueError(f"DAG contains a dependency cycle: {' -> '.join(stack[start:] + [name])}")
        state[name] = "visiting"
        stack.append(name)
        for dependency in dependencies[name]:
            visit(dependency)
        stack.pop()
        state[name] = "done"

    for name in sorted(names):
        visit(name)

    def depends_on(target, source):
        pending = list(dependencies[target])
        seen = set()
        while pending:
            candidate = pending.pop()
            if candidate == source:
                return True
            if candidate not in seen:
                seen.add(candidate)
                pending.extend(dependencies[candidate])
        return False

    for index, link in enumerate(links):
        source = link["source_node"]
        target = link["target_node"]
        if source not in names:
            raise ValueError(f"workflow.links[{index}] references missing source module: {source}")
        if target not in names:
            raise ValueError(f"workflow.links[{index}] references missing target module: {target}")
        if not depends_on(target, source):
            raise ValueError(f"workflow.links[{index}] source must be a dependency of its target: {source} -> {target}")


def _configure_dag_workflow(args):
    workflow_path = os.path.realpath(args.workflow)
    workflow = _load_mapping(workflow_path)
    _validate_keys(
        workflow,
        {
            "schema_version",
            "output_directory",
            "cache_directory",
            "fail_fast",
            "dot",
            "provenance",
            "modules",
            "links",
        },
        "workflow",
    )
    if workflow.get("schema_version") != 1:
        raise ValueError("DAG workflow requires schema_version: 1")
    modules = workflow.get("modules")
    if not isinstance(modules, list) or not modules:
        raise ValueError("DAG workflow requires a non-empty modules list")
    links = workflow.get("links", [])
    if not isinstance(links, list):
        raise TypeError("workflow.links must be a list")

    base = os.path.dirname(workflow_path)
    default_output = _resolve_config_path(base, workflow.get("output_directory"))
    default_cache = _resolve_config_path(base, workflow.get("cache_directory"))
    configured_dot = _resolve_config_path(base, workflow.get("dot"))
    configured_provenance = _resolve_config_path(base, workflow.get("provenance"))
    if configured_dot and configured_dot == configured_provenance:
        raise ValueError("workflow.dot and workflow.provenance must not use the same path")
    configured = []
    names = set()
    allowed_module_keys = {
        "module",
        "name",
        "dependencies",
        "isolated",
        "params",
        "param_file",
        "output_directory",
        "cache_directory",
    }

    workflow_fail_fast = workflow.get("fail_fast", True)
    if not isinstance(workflow_fail_fast, bool):
        raise TypeError("workflow.fail_fast must be a boolean")
    for index, item in enumerate(modules):
        if not isinstance(item, dict):
            raise TypeError(f"workflow.modules[{index}] must be a mapping")
        _validate_keys(item, allowed_module_keys, f"workflow.modules[{index}]")
        class_name = item.get("module")
        name = item.get("name")
        if not isinstance(class_name, str) or not class_name:
            raise ValueError(f"workflow.modules[{index}].module must be a non-empty string")
        if not isinstance(name, str) or not name:
            raise ValueError(f"workflow.modules[{index}].name must be a non-empty string")
        if name in names:
            raise ValueError(f"duplicate DAG module name: {name}")
        names.add(name)
        dependencies = item.get("dependencies", [])
        if not isinstance(dependencies, list) or not all(isinstance(value, str) and value for value in dependencies):
            raise TypeError(f"workflow.modules[{index}].dependencies must be a list of names")
        params = item.get("params", {})
        if not isinstance(params, dict):
            raise TypeError(f"workflow.modules[{index}].params must be a mapping")
        isolated = item.get("isolated", False)
        if not isinstance(isolated, bool):
            raise TypeError(f"workflow.modules[{index}].isolated must be a boolean")
        configured.append({
            "class_name": class_name,
            "name": name,
            "dependencies": dependencies,
            "isolated": isolated,
            "params": params,
            "param_file": item.get("param_file"),
            "output_directory": item.get("output_directory"),
            "cache_directory": item.get("cache_directory"),
        })

    configured_links = []
    for index, link in enumerate(links):
        if not isinstance(link, dict):
            raise TypeError(f"workflow.links[{index}] must be a mapping")
        _validate_keys(link, {"from", "to"}, f"workflow.links[{index}]")
        source_node, source_param = _split_parameter_ref(link.get("from"), f"workflow.links[{index}].from")
        target_node, target_param = _split_parameter_ref(link.get("to"), f"workflow.links[{index}].to")
        configured_links.append({
            "source_node": source_node,
            "source_param": source_param,
            "target_node": target_node,
            "target_param": target_param,
        })
    _validate_dag_structure(configured, configured_links)

    controller = _load_controller(args.json, getattr(args, "require_signed", False))
    for item in configured:
        handle = controller.register_module(item["class_name"], item["name"])
        output = _resolve_config_path(base, item["output_directory"]) or default_output
        cache = _resolve_config_path(base, item["cache_directory"]) or default_cache
        if output:
            handle.set_output_directory(output)
        if cache:
            handle.set_cache_directory(cache)
        param_file = _resolve_config_path(base, item["param_file"])
        if param_file:
            _apply_parameters(handle, _load_mapping(param_file))
        _apply_parameters(handle, item["params"])

    for item in configured:
        controller.add_module_to_dag(item["name"], item["dependencies"], isolated=item["isolated"])

    for link in configured_links:
        controller.link_dag_parameter(
            link["source_node"], link["source_param"], link["target_node"], link["target_param"]
        )

    fail_fast_override = getattr(args, "fail_fast", None)
    fail_fast = workflow_fail_fast if fail_fast_override is None else fail_fast_override
    return {
        "controller": controller,
        "workflow": workflow,
        "workflow_path": workflow_path,
        "base": base,
        "modules": configured,
        "links": configured_links,
        "default_output": default_output,
        "default_cache": default_cache,
        "dot": configured_dot,
        "provenance": configured_provenance,
        "fail_fast": fail_fast,
    }


def cmd_dag_validate(args) -> None:
    with _redirect_stdout_to_stderr(args.json):
        configured = _configure_dag_workflow(args)
    payload = {
        "valid": True,
        "workflow": configured["workflow_path"],
        "modules": len(configured["modules"]),
        "links": len(configured["links"]),
        "output_directory": configured["default_output"],
        "cache_directory": configured["default_cache"],
    }
    if args.json:
        _emit(payload, True)
    else:
        print(
            f"Valid workflow: {payload['modules']} module(s), {payload['links']} parameter link(s)."
        )


def cmd_dag_run(args) -> None:
    with _redirect_stdout_to_stderr(args.json):
        configured = _configure_dag_workflow(args)
        controller = configured["controller"]
        workflow = configured["workflow"]
        base = configured["base"]

        provenance_path = getattr(args, "provenance", None) or workflow.get("provenance")
        resolved_provenance = (
            _resolve_config_path(base, provenance_path)
            if provenance_path
            else None
        )
        if resolved_provenance:
            result = controller.run_dag(
                fail_fast=configured["fail_fast"], provenance_path=resolved_provenance
            )
        else:
            result = controller.run_dag(fail_fast=configured["fail_fast"])
        dot_path = args.dot or workflow.get("dot")
        if dot_path:
            resolved_dot = _resolve_config_path(base, dot_path)
            os.makedirs(os.path.dirname(resolved_dot), exist_ok=True)
            controller.get_dag().dump_dot(resolved_dot)

    nodes = [
        {
            "name": node.name,
            "status": getattr(node.status, "name", str(node.status).rsplit(".", 1)[-1]).title(),
            "message": node.message,
        }
        for node in result.nodes
    ]
    payload = {
        "succeeded": result.succeeded(),
        "failed": result.failed(),
        "nodes": nodes,
        "provenance": getattr(controller, "last_workflow_provenance_path", ""),
    }
    if args.json:
        _emit(payload, True)
    else:
        for node in nodes:
            detail = f": {node['message']}" if node["message"] else ""
            print(f"{node['name']}: {node['status']}{detail}")
        if payload["provenance"]:
            print(f"Provenance: {payload['provenance']}")
    if result.failed():
        raise SystemExit(1)
