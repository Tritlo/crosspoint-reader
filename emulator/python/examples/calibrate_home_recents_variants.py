from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Measure Home while removing every recent EPUB, then restore the list."""
    removed_paths: list[str] = []

    device.select_files_on_home()
    device.sleep(2)
    device.press("down", hold_ms=100, settle_ms=100)
    entering_recents = device.mark("home-recents-enter")
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.wait_for_log(
        "Entering activity: RecentBooks",
        after_host_time_ns=entering_recents.host_time_ns,
        timeout_s=30,
    )
    device.sleep(2)

    for index in range(10):
        removing = device.mark(f"home-recents-remove-{index:02d}")
        device.press("confirm", hold_ms=1_200, settle_ms=100)
        try:
            device.wait_for_log(
                "Entering activity: Confirmation",
                after_host_time_ns=removing.host_time_ns,
                timeout_s=3,
            )
        except TimeoutError:
            break
        device.sleep(1)
        device.press("right", hold_ms=100, settle_ms=100)
        removed = device.wait_for_log(
            "Removed from recents: ",
            after_host_time_ns=removing.host_time_ns,
            timeout_s=30,
        )
        removed_path = removed.line.split("Removed from recents: ", 1)[1]
        if not removed_path.lower().endswith(".epub"):
            raise RuntimeError(f"cannot restore non-EPUB recent entry: {removed_path}")
        removed_paths.append(removed_path)
        device.sleep(1)

    if not removed_paths:
        raise RuntimeError("no recent EPUBs were available to measure")

    empty_home = device.mark(f"home-recents-empty-{len(removed_paths):02d}")
    device.press("back", hold_ms=100, settle_ms=100)
    device.wait_for_log(
        "Entering activity: Home",
        after_host_time_ns=empty_home.host_time_ns,
        timeout_s=30,
    )
    device.sleep(3)

    for remaining, path in enumerate(reversed(removed_paths), start=1):
        restoring = device.mark(f"home-recents-restore-{remaining:02d}")
        device.open_epub(path)
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

    device.mark("home-recents-restored")
