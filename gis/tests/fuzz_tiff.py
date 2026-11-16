#!/usr/bin/env python3
"""Throw randomly damaged TIFFs at the reader; anything but a GeoError is a bug.

    python3 tests/fuzz_tiff.py [SEED] [FILES_PER_COMBINATION]

Each combination of compression, layout and band count is built, then bytes are
overwritten at random. Prints the kinds of unexpected exceptions it saw (an
empty {} means every failure was a clean GeoError). The default of 3000 per
combination covers about 72,000 files.
"""

import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from geo import GeoError, GeoTiff  # noqa: E402
from writers import write_tiff  # noqa: E402

VALUES = [(x * 7 + y * 3) % 250 for y in range(10) for x in range(13)]


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    per = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
    rnd = random.Random(seed)
    bad, total = {}, 0
    for comp in (1, 5, 8):
        for layout in (dict(rows_per_strip=3), dict(tile=(4, 4))):
            for bands in (1, 3):
                good = write_tiff(13, 10, [VALUES] * bands, compression=comp,
                                  predictor=2 if comp != 1 else 1,
                                  geo=dict(scale=(1, 1), tie=(0, 0)), nodata=-1, **layout)
                for _ in range(per):
                    data = bytearray(good)
                    for _ in range(rnd.randint(1, 8)):
                        data[rnd.randrange(len(data))] = rnd.randrange(256)
                    total += 1
                    try:
                        t = GeoTiff(bytes(data))
                        if t.width * t.height < 200000 and t.bands < 8:
                            t.read_band(0)
                            t.stats()
                            if t.georef:
                                t.sample(1.5, -1.5)
                    except GeoError:
                        pass
                    except Exception as e:  # noqa: BLE001 - this is the thing being hunted
                        key = f"{type(e).__name__}: {str(e)[:70]}"
                        bad[key] = bad.get(key, 0) + 1
    print(f"{total} damaged files, unexpected exceptions: {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
