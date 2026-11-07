"""Keep LLVM links within Windows' process command-line limit."""
import subprocess
import sys
from pathlib import Path


def command(arguments):
    arguments = [str(value) for value in arguments]
    if sys.platform != 'win32' or len(subprocess.list2cmdline(arguments)) < 30000:
        return arguments
    output = Path(arguments[arguments.index('-o') + 1])
    response = output.with_suffix(output.suffix + '.rsp')
    # LLVM accepts quoted forward-slash paths in both its COFF and ELF drivers.
    response.write_text('\n'.join('"' + value.replace('\\', '/').replace('"', '\\"') + '"'
                                  for value in arguments[1:]) + '\n', encoding='utf-8')
    return [arguments[0], '@' + str(response)]
