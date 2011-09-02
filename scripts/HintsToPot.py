# Helps converting hints.ini into POT

import sys

from configparser import ConfigParser
from pathlib import Path


def write_to_pot(path: Path, data: dict[str, str]):
    with open(path, "a+t") as pot_file:
        for key in data.keys():
            print(
                f"\n#: resources/data/hints.ini: [{ key }]\nmsgid \"{ data[key]['text'] }\"\nmsgstr \"\"",
                file=pot_file,
            )


def main():
    if len(sys.argv) != 3:
        print("HINTS_TO_POT FAILED: WRONG NUM OF ARGS")
        exit(-1)
    path_to_ini = Path(sys.argv[1]).parent / "resources" / "data" / "hints.ini"
    path_to_pot = Path(sys.argv[2]).parent / "i18n" / "OrcaSlicer.pot"
    if not path_to_ini.exists():
        print("HINTS_TO_POT FAILED: PATH TO INI DOES NOT EXISTS")
        print(str(path_to_ini))
        exit(-1)
    config = ConfigParser()
    with open(path_to_ini) as hints_file:
        config.read_file(hints_file)
    write_to_pot(path_to_pot, config._sections)
    print("HINTS_TO_POT SUCCESS")
    exit(0)


if __name__ == "__main__":
    main()
