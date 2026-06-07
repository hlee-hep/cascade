FAQ (Example)

Q: Why is my plugin not loading?
A: Run `cascade doctor plugins`; check for `plugin_pubkey.pem`, signed `plugin_manifest.json`, matching file hashes, and C++ ABI compatibility.

Q: Do I need to rebuild plugins after updating Cascade?
A: Yes, if the ABI version changed.

Q: Why are some Python plugins missing?
A: Only files ending with `module.py` and listed in a verified `plugin_manifest.json` are scanned.

Q: Can C++ and Python plugins use the same module name?
A: No. Module names must be globally unique; run `cascade doctor plugins` to detect duplicates.
