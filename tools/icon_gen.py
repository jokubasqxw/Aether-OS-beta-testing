#!/usr/bin/env python3
"""
icon_gen.py - Generate sample icons for testing the icon system.

Usage:
    python3 tools/icon_gen.py

Creates sample icons in icons/ directory:
    terminal.png   - Terminal icon (green)
    browser.png    - Browser icon (blue)
    settings.png   - Settings icon (orange)
    taskmanager.png - Task Manager icon (red)
"""

import os
from PIL import Image, ImageDraw
import os

OUTPUT_DIR = "icons"

def create_icon(name, color, size=32):
    """Create a simple colored square with a label."""
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    path = os.path.join(OUTPUT_DIR, f"{name}.png")

    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Draw a colored square
    draw.rectangle([0, 0, size-1, size-1], fill=color)

    # Add a simple label or symbol
    draw.text((size//4, size//4), get_label(name), fill=(255, 255, 255, 255))

    img.save(path)
    print(f"Created {path}")

def get_label(name):
    """Get a simple label for each icon type."""
    labels = {
        "terminal": "T",
        "browser": "B",
        "settings": "S",
        "taskmanager": "T",
        "apphub": "A",
    }
    return labels.get(name, "?")

def main():
    print("Generating sample icons...")
    print(f"Output directory: {os.path.abspath(OUTPUT_DIR)}")

    # Terminal - green
    create_icon("terminal", (0, 128, 0, 255))

    # Browser - blue
    create_icon("browser", (0, 0, 128, 255))

    # Settings - orange
    create_icon("settings", (255, 128, 0, 255))

    # Task Manager - red
    create_icon("taskmanager", (128, 0, 0, 255))

    # App Hub - purple
    create_icon("apphub", (128, 0, 128, 255))

    print("\nIcons generated. Next steps:")
    print("  1. Replace icons with your own images in the icons/ directory")
    print("  2. Run: python3 tools/icon_conv.py <your_image> output.h IconName")
    print("  3. Add the generated .h file to your build")

if __name__ == "__main__":
    main()
