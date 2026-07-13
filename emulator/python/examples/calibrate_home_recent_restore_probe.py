from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Remove and restore the first recent book to validate the reversible workflow."""
    device.select_files_on_home()
    device.sleep(2)
    device.press("down", hold_ms=100, settle_ms=100)

    entering_recents = device.mark("recent-restore-probe-enter")
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.wait_for_log(
        "Entering activity: RecentBooks",
        after_host_time_ns=entering_recents.host_time_ns,
        timeout_s=30,
    )
    device.sleep(2)

    removing = device.mark("recent-restore-probe-remove")
    device.press("confirm", hold_ms=1_200, settle_ms=100)
    device.wait_for_log(
        "Entering activity: Confirmation",
        after_host_time_ns=removing.host_time_ns,
        timeout_s=30,
    )
    device.sleep(2)
    device.press("right", hold_ms=100, settle_ms=100)
    removed = device.wait_for_log(
        "Removed from recents: ",
        after_host_time_ns=removing.host_time_ns,
        timeout_s=30,
    )
    removed_path = removed.line.split("Removed from recents: ", 1)[1]
    if not removed_path.lower().endswith(".epub"):
        raise RuntimeError(f"cannot restore non-EPUB recent entry: {removed_path}")

    device.sleep(3)
    returning_home = device.mark("recent-restore-probe-one-removed")
    device.press("back", hold_ms=100, settle_ms=100)
    device.wait_for_log(
        "Entering activity: Home",
        after_host_time_ns=returning_home.host_time_ns,
        timeout_s=30,
    )
    device.sleep(3)

    restoring = device.mark("recent-restore-probe-restoring")
    device.open_epub(removed_path)
    device.wait_for_log(
        "Entering activity: EpubReader",
        after_host_time_ns=restoring.host_time_ns,
        timeout_s=30,
    )
    device.wait_for_log("Page render", after_host_time_ns=restoring.host_time_ns, timeout_s=120)
    device.press("back", hold_ms=100, settle_ms=100)
    device.wait_for_log(
        "Entering activity: Home",
        after_host_time_ns=restoring.host_time_ns,
        timeout_s=30,
    )
    device.sleep(3)
    device.mark("recent-restore-probe-restored")
