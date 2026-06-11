"""Make the releasetools modules importable as top-level names in tests.

The scripts import siblings as ``from release_notes import ...`` so they work
when run directly from the package directory (sys.path[0] is the script dir).
Tests run from the repo root, so add the package dir to sys.path here.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
