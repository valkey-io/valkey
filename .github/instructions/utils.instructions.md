---
applyTo:
  - "utils/**/*"
---

# Valkey Utilities Review Standards

Apply these standards to utility scripts and tools.

## 1. Script Quality
- **Portability:** Scripts should work across common platforms (Linux, macOS).
- **Error Handling:** Check for errors and provide clear error messages.
- **Documentation:** Include usage instructions in comments or help text.

## 2. Code Standards
- **Python:** Follow PEP 8 style guidelines.
- **Ruby:** Follow standard Ruby conventions.
- **Shell:** Use shellcheck-compatible patterns.
- **C Tools:** Follow LLVM style (4-space indent, no tabs).

## 3. Best Practices
- **Dependencies:** Minimize external dependencies.
- **Safety:** Validate inputs and avoid destructive operations without confirmation.
- **Maintainability:** Keep utilities simple and well-commented.
