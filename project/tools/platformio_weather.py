"""generate the weather header before PlatformIO scans/compiles sources."""
from pathlib import Path
import sys

from SCons.Script import COMMAND_LINE_TARGETS

# obtain the active SCons environment.
Import("env")
# use the active PlatformIO environment and project-owned weather helper.
tools_dir = Path(env.subst("$PROJECT_DIR")) / "tools"
sys.path.insert(0, str(tools_dir))
from upload_weather import WeatherError, prepare

# prepare weather before dependency scanning and compilation.
try:
    header = prepare(env.subst("$PROJECT_DIR"), env.subst("$BUILD_DIR"), COMMAND_LINE_TARGETS)
    # add the generated header path and rebuild dependency when generation runs.
    if header is not None:
        env.AppendUnique(CPPPATH=[str(header.parent)])
        # track the header explicitly and through the include scanner.
        env.Depends("$BUILD_DIR/src/main.cpp.o", env.File(str(header)))
# stop the build when weather preparation fails.
except (WeatherError, OSError, ValueError) as error:
    print("Upload weather error: %s" % error)
    env.Exit(1)
