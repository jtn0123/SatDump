# Fixture for test_check_deps.py

A pretend DEPENDENCIES.md: every listed library must be mentioned by its first path, and a table
row that names that path must start a cell with the listed version.

| Library | Path | Version |
|---|---|---|
| alpha | `lib/alpha` | 1.2.3 (from `alpha.h`) |
| single | `lib/single.h` | unversioned |
