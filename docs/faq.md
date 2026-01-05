FAQ (Example)

Q: Why is my plugin not loading?
A: Check for `plugin_pubkey.pem`, ensure the `.sig` exists, and verify ABI matches.

Q: Do I need to rebuild plugins after updating Cascade?
A: Yes, if the ABI version changed.

Q: Why are some Python plugins missing?
A: Only files ending with `module.py` are scanned, and signatures are required when a key is present.
