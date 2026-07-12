from collections.abc import Iterator
from pathlib import Path

import pytest

from .client import DeviceProfile, Emulator


@pytest.fixture(params=("x3", "x4"))
def device_profile(request: pytest.FixtureRequest) -> DeviceProfile:
    return request.param  # type: ignore[no-any-return]


@pytest.fixture
def emulator_device(device_profile: DeviceProfile, tmp_path: Path) -> Iterator[Emulator]:
    with Emulator.launch(device_profile, artifacts=tmp_path / "run") as device:
        yield device
