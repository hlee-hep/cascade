import os
import sys

from .common import _install_sigint_handler, _log
from .parser import build_parser
from .system import cmd_macro_run


_BANNER = r"""   _________   _____ _________    ____  ______
  / ____/   | / ___// ____/   |  / __ \/ ____/
 / /   / /| | \__ \/ /   / /| | / / / / __/
/ /___/ ___ |___/ / /___/ ___ |/ /_/ / /___
\____/_/  |_/____/\____/_/  |_/_____/_____/"""
_TAGLINE = "Composable Analysis with Secure Caching And DAG Execution"


def _banner_text(color: bool = False) -> str:
    if not color:
        return f"{_BANNER}\n{_TAGLINE}"
    return f"\033[1;36m{_BANNER}\033[0m\n\033[2m{_TAGLINE}\033[0m"


def _show_banner(argv) -> bool:
    return sys.stdout.isatty() and (not argv or argv in (["-h"], ["--help"]))


def _print_banner() -> None:
    color = "NO_COLOR" not in os.environ and os.environ.get("TERM") != "dumb"
    print(_banner_text(color), file=sys.stdout)
    print(file=sys.stdout)


def main(argv=None) -> None:
    _install_sigint_handler()
    argv = sys.argv[1:] if argv is None else argv
    if _show_banner(argv):
        _print_banner()
    parser = build_parser()
    if not argv:
        parser.print_help()
        return
    args = parser.parse_args(argv)
    if getattr(args, "func", None) == cmd_macro_run and not args.macro:
        parser.error("--macro or a subcommand is required")

    try:
        args.func(args)
    except KeyboardInterrupt:
        _log("WARNING", "Interrupted")
        sys.exit(130)
    except SystemExit:
        raise
    except Exception as e:
        _log("ERROR", f"{e}")
        sys.exit(1)
