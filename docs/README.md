# Starling Project Notes

This directory contains maintainer-oriented notes generated from the current
repository state. They are intended to make future code changes faster and less
risky.

- [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md): what this repository is and where to
  start.
- [ARCHITECTURE.md](ARCHITECTURE.md): main modules, runtime flows, and ownership
  boundaries.
- [BUILD_TEST_RUN.md](BUILD_TEST_RUN.md): dependencies, build commands, test
  commands, and current local build blockers.
- [DATA_FORMATS.md](DATA_FORMATS.md): binary data, truthset, index, frequency,
  and partition file conventions.
- [WORKFLOWS.md](WORKFLOWS.md): common benchmark and experiment flows.
- [INDEX_STORAGE.md](INDEX_STORAGE.md): required location and naming convention
  for generated index files.
- [DATASET_BENCHMARKS.md](DATASET_BENCHMARKS.md): dataset matrix and commands
  for running Starling across the local DiskANN datasets.
- [DISKANN_BEATS_STARLING.md](DISKANN_BEATS_STARLING.md): design notes for
  improving DiskANN beam search until it can beat Starling page-search baselines.
- [KNOWN_ISSUES.md](KNOWN_ISSUES.md): current risks and sharp edges observed in
  this checkout.
- [AI_MAINTENANCE_GUIDE.md](AI_MAINTENANCE_GUIDE.md): practical rules for future
  AI-assisted edits in this codebase.
