"""set and verify the RTC only after a successful firmware upload."""
from pathlib import Path
import sys

# obtain the active SCons environment.
Import("env")


# provision fresh UTC over USB after successful flashing.
def provision_rtc(source, target, env):
    tools_dir = Path(env.subst("$PROJECT_DIR")) / "tools"
    sys.path.insert(0, str(tools_dir))
    from sync_rtc import RtcSyncError, synchronize

    # rediscover the application port after renumbering and verify RTC ticking.
    try:
        synchronize()
    # fail the post-upload action clearly if RTC setup is unsuccessful.
    except RtcSyncError as error:
        print("RTC provisioning failed after firmware upload: %s" % error)
        print("Firmware was uploaded. Close serial monitors, then rerun tools/sync_rtc.py with PlatformIO's Python.")
        return 1
    return 0


# attach RTC provisioning to the upload target only.
env.AddPostAction("upload", provision_rtc)
