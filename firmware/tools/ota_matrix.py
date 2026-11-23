"""Compare completed candidate manifests and images in one build batch with a release."""
import argparse
import hashlib
import json
from pathlib import Path

from firmware_budget import MIN_OTA_HEADROOM
from p4_image_compat import image_application_version, image_revision_range

REVISIONS = {'v1': (1, 199), 'v3': (300, 399)}
TARGET_BUFFER = 64 * 1024


def compare(release, reports, images):
    rows = {}
    for board in ('wifi6', 'pico'):
        for family, revisions in REVISIONS.items():
            profile = f'{board}-{family}'
            matches = [f for f in release['firmware'] if f['boardModel'] == board
                       and (f['minChipRevision'], f['maxChipRevision']) == revisions]
            if len(matches) != 1:
                raise ValueError(f'{profile}: missing or ambiguous release baseline')
            baseline = matches[0]['bytes']
            if type(baseline) is not int or baseline <= 0:
                raise ValueError(f'{profile}: invalid release size')
            row = rows[profile] = dict(status='pending', releasedBytes=baseline)
            manifest = reports / profile / 'manifest.json'
            if not manifest.exists():
                continue
            record = json.loads(manifest.read_text())
            binary = (images / f'{profile}-firmware.bin').read_bytes()
            size = len(binary)
            digest = hashlib.sha256(binary).hexdigest()
            version = image_application_version(binary)
            budget = record['budget']
            slot = budget['otaBytes']
            if ((record['board'], record['family']) != (board, family)
                    or record['files']['firmware.bin'] != {'bytes': size, 'sha256': digest}
                    or size != budget['appBytes'] or slot - size != budget['otaHeadroomBytes']
                    or image_revision_range(binary) != revisions or not version.endswith('-' + board)):
                raise ValueError(f'{profile}: manifest, budget and ESP image disagree')
            margin = slot - size - MIN_OTA_HEADROOM
            row.update(status='pass' if margin >= 0 else 'fail', appBytes=size,
                       deltaBytes=size-baseline, slotBytes=slot, headroomBytes=slot-size,
                       aboveMinimumBytes=margin, targetMet=margin >= TARGET_BUFFER,
                       sha256=digest, commit=record['commit'], dirty=record['dirty'])
    return dict(release=release['desktopVersion'], minimumBytes=MIN_OTA_HEADROOM,
                targetHeadroomBytes=MIN_OTA_HEADROOM + TARGET_BUFFER, boards=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release-index', required=True, type=Path)
    parser.add_argument('--reports', required=True, type=Path)
    parser.add_argument('--images', required=True, type=Path)
    parser.add_argument('--require-complete', action='store_true')
    args = parser.parse_args()
    result = compare(json.loads(args.release_index.read_text()), args.reports, args.images)
    print(json.dumps(result, indent=2))
    return any(row['status'] == 'fail' or args.require_complete and row['status'] != 'pass'
               for row in result['boards'].values())


if __name__ == '__main__':
    raise SystemExit(main())
