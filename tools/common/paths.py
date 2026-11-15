"""Default project-local dependency and release directories."""
import os
from pathlib import Path
import sys

PROJECT = Path(__file__).resolve().parents[2]
DEFAULT_OPENWRT = PROJECT / 'deps/openwrt'
DIST = PROJECT / 'dist'


def require_linux():
    if not sys.platform.startswith('linux'):
        raise SystemExit('Builds require Linux. On Windows, run this command inside WSL.')


def build_environment():
    env = os.environ.copy()
    env['PATH'] = '/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin'
    env.update(LC_ALL='C', LANG='C', TZ='UTC')
    return env
