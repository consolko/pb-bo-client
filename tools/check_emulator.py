"""Compatibility entry point: isolated contract checks now use client_check.app."""
import runpy
from pathlib import Path

# Production has no demo mode or HTTP exception. The check executable owns its fixture.
runpy.run_path(str(Path(__file__).with_name("check_stage3.py")), run_name="__main__")
