---
applyTo:
  - "tests/**/*.tcl"
---

# Valkey Integration Test Review Standards

Apply these standards to Tcl-based integration tests (from DEVELOPMENT_GUIDE.md).

## 1. Test Organization
- **Cluster:** Use `tests/unit/cluster/` (NOT legacy `tests/cluster/` which is deprecated).
- **Coverage:** All contributions should include tests. New commands require integration tests.
- **Naming:** Use descriptive test names that explain what is being tested.

## 2. Test Quality
- **Isolation:** Tests should not depend on execution order.
- **Cleanup:** Ensure proper cleanup of resources and temporary files.
- **Assertions:** Use clear assertions with meaningful error messages.

## 3. Best Practices
- **Readability:** Keep tests simple and focused on one scenario.
- **Reliability:** Avoid timing-dependent tests; use proper synchronization.
- **Documentation:** Comment complex test scenarios to explain intent.
