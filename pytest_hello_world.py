# SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0

import pytest
from pytest_embedded_idf.dut import IdfDut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.generic
@idf_parametrize('target', ['esp32s3'], indirect=['target'])
def test_smart_light_boots(dut: IdfDut) -> None:
    dut.expect('ESP32-S3 zero-cross dimmer demo')
    dut.expect('zero-cross GPIO=3, MOC GPIO=8')
