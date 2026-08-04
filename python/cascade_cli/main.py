import sys

from .common import _install_sigint_handler, _log
from .parser import build_parser
from .system import cmd_macro_run


def main(argv=None) -> None:
    _install_sigint_handler()
    argv = sys.argv[1:] if argv is None else argv
    parser = build_parser()
    args = parser.parse_args(argv)
    if getattr(args, "func", None) == cmd_macro_run and not args.macro:
        parser.error("--macro or a subcommand is required")

    try:
        args.func(args)
    except KeyboardInterrupt:
        _log("WARN", "Interrupted")
        sys.exit(130)
    except SystemExit:
        raise
    except Exception as e:
        _log("ERROR", f"{e}")
        sys.exit(1)
