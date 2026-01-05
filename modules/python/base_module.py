from cascade import is_interrupted, log, log_level
import cascade
import hashlib
import json
import os

class base_module:
    _hash_cache = set()
    _hash_cache_loaded = False

    def __init__(self):
        self.params = {}
        self.status = "Pending"
        self.code_version_hash = ""
        self.basename = ""
        self.m_name = ""
        self.summary = ""
        self.tags = []

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

    def get_metadata(self):
        return {
            "name": self.get_basename() or self.__class__.__name__,
            "version": getattr(cascade, "__version__", ""),
            "summary": self.summary,
            "tags": list(self.tags),
        }

    def print_description(self):
        raise NotImplementedError("PythonModuleBase: print_description() must be implemented by subclass")

    def run(self):
        self.set_status("Initializing")
        self.init()
        
        if self.check_interrupt():
            return

        self._load_hash_cache()
        if self.params.get("dry_run", False):
            self.set_status("Skipped")
            return

        if not self.params.get("force_run", False):
            snapshot_hash = self._compute_snapshot_hash()
            if snapshot_hash in base_module._hash_cache:
                log(log_level.ERROR, self.m_name, "Duplication of hash is detected.")
                self.set_status("Skipped")
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
            snapshot_hash = self._compute_snapshot_hash()
            base_module._hash_cache.add(snapshot_hash)
            self._save_hash_cache()
            self.set_status("Done")

    def init(self):
        raise NotImplementedError("PythonModuleBase: init() must be implemented by subclass")

    def execute(self):
        raise NotImplementedError("PythonModuleBase: execute() must be implemented by subclass")
    
    def finalize(self):
        raise NotImplementedError("PythonModuleBase: finalize() must be implemented by subclass")

    def _compute_snapshot_hash(self):
        payload = {
            "basename": self.basename,
            "code_hash": self.code_version_hash,
            "params": self.params,
        }
        raw = json.dumps(payload, sort_keys=True, default=str)
        return hashlib.sha256(raw.encode("utf-8")).hexdigest()

    def _hash_cache_path(self):
        base = os.path.join(os.path.expanduser("~"), ".cache", "cascade")
        os.makedirs(base, exist_ok=True)
        return os.path.join(base, "py_module_hashes.json")

    def _load_hash_cache(self):
        if base_module._hash_cache_loaded:
            return
        base_module._hash_cache_loaded = True
        path = self._hash_cache_path()
        if not os.path.exists(path):
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
                if isinstance(data, list):
                    base_module._hash_cache.update(data)
        except Exception:
            return

    def _save_hash_cache(self):
        path = self._hash_cache_path()
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(sorted(base_module._hash_cache), f, indent=2)
        except Exception:
            return
