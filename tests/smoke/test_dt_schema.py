# SPDX-License-Identifier: GPL-2.0-or-later
#
# Validate the machine's built-in device tree against the Linux kernel's
# bindings with dt-schema's dt-validate. Runs from 'make check-dt', which
# fetches the bindings at a pinned tag and points DT_SCHEMA at them
# processed; skipped otherwise.

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_hello import GUEST, QEMU, TIMEOUT

DT_SCHEMA = os.environ.get("DT_SCHEMA")

# Configurations whose trees differ: core count, EL3, RAM above 1 GiB
CONFIGS = (
    ("raspi5b", "-smp", "1", "-m", "1G"),
    ("raspi5b", "-smp", "4", "-m", "8G"),
    ("raspi5b,secure=on", "-smp", "2", "-m", "2G"),
)

# Messages that are expected, each with the reason. Anything else fails.
KNOWN = (
    # The firmware publishes the board revision here and the Raspberry Pi
    # kernel reads it; no binding describes /system
    re.compile(r"system: linux,revision: .* is not of type"),
    # The firmware node as in Linux's own bcm2712-rpi-5-b-ovl-rp1.dts, whose
    # binding does not allow for it yet
    re.compile(r"soc@107c000000 \(simple-bus\): firmware: 'ranges' is a "
               r"required property"),
    re.compile(r"firmware \(raspberrypi,bcm2835-firmware\): "
               r"'#address-cells', '#size-cells', 'dma-ranges' do not match"),
)


@unittest.skipUnless(DT_SCHEMA, "run through 'make check-dt'")
@unittest.skipUnless(QEMU.exists() and GUEST.exists(),
                     "build QEMU and the guest first (make build guest)")
class DtSchemaTest(unittest.TestCase):

    def test_builtin_dtb_validates(self):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        dtbs = []
        for i, (machine, *args) in enumerate(CONFIGS):
            dtb = tmp / f"raspi5b-{i}.dtb"
            subprocess.run([str(QEMU), "-M", f"{machine},dumpdtb={dtb}",
                            "-display", "none", "-kernel", str(GUEST), *args],
                           stdin=subprocess.DEVNULL, capture_output=True,
                           check=True, timeout=TIMEOUT)
            dtbs.append(str(dtb))

        result = subprocess.run(["dt-validate", "-s", DT_SCHEMA, *dtbs],
                                capture_output=True, text=True,
                                timeout=10 * TIMEOUT)
        messages = [line for line in
                    (result.stdout + result.stderr).splitlines()
                    if line.strip() and not line.startswith("\tfrom schema")]
        unexpected = [m for m in messages
                      if not any(k.search(m) for k in KNOWN)]
        self.assertEqual(unexpected, [], "\n".join(unexpected))


if __name__ == "__main__":
    unittest.main()
