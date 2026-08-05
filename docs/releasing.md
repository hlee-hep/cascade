# Preparing a release

Cascade versions the framework, plugin ABI, and document schemas independently.
Read [Versioning and compatibility](versioning.md) before changing any version.

## 0.3 release checklist

### Contract

- [ ] `include/Version.hh`, the README, quickstart, and migration guide report the intended semantic version.
- [ ] `CASCADE_PLUGIN_ABI_VERSION` changes only when the public C++ binary contract changes.
- [ ] Plugin manifest, analysis configuration, cache, and provenance schema versions remain compatible or have migration notes.
- [ ] Public C++ headers and Python control surfaces match their documentation.
- [ ] `CHANGELOG.md` describes user-visible changes and breaking changes.
- [x] The repository uses the MIT License.

### Verification

- [ ] `scons -j2` completes from a clean checkout with documented dependencies.
- [ ] `scons test -j2` passes.
- [ ] `cascade info` reports the expected version, ABI integer, and ABI tag.
- [ ] `cascade doctor env`, `cascade doctor runtime`, and `cascade doctor plugins`
  report no unexpected failures.
- [ ] `cascade dag validate examples/plugins/mixed_pipeline/workflow.yaml` passes after installing the example plugin.
- [ ] The mixed C++/Python workflow completes in both in-process and isolated modes.
- [ ] A second identical run produces expected snapshot cache hits.
- [ ] `cascade cache list` links those snapshots to existing provenance manifests.
- [ ] Failure paths roll staged outputs back and still record terminal provenance.

### Distribution

- [ ] Build and install Cascade into an empty staging prefix.
- [ ] Build plugins against that installed SDK rather than the source tree.
- [ ] Regenerate plugin manifests after the final build.
- [ ] Re-sign distributed plugin manifests and verify them with provisioned public keys.
- [ ] Review installation paths and runtime loader instructions on a fresh shell.
- [ ] Create the release tag only after the staged artifacts pass verification.

## Release commands

The exact prefix is operator-specific; an isolated staging prefix keeps the
verification reproducible:

```bash
scons -j2
scons test -j2
scons install PREFIX=/tmp/cascade-release

/tmp/cascade-release/bin/cascade info
/tmp/cascade-release/bin/cascade doctor env
/tmp/cascade-release/bin/cascade doctor runtime
/tmp/cascade-release/bin/cascade doctor plugins
```

Do not publish private signing keys or copy them into a plugin package. Only
publisher public keys belong in a deployment trust store.
