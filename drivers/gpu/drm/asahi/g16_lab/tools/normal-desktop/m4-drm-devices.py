#!/usr/bin/env python3
"""Keep the boot-selected internal KMS primary and include a live external card."""
import os
from pathlib import Path

cards = os.environ['AQ_DRM_DEVICES'].split(':')
for card in sorted(Path('/sys/class/drm').glob('card[0-9]*')):
    if not card.name[4:].isdigit():
        continue
    if 'm4-dcpext-dma-probe' not in card.resolve().parts:
        continue
    connector = card / (card.name + '-DP-1')
    if not connector.exists():
        continue
    device = '/dev/dri/' + card.name
    if device not in cards:
        cards.append(device)
print(':'.join(cards))
