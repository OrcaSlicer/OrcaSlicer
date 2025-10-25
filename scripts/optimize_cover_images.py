#!/usr/bin/env python3
"""
Optimize cover images:
1. Scale the image to maintain proper margins around the content.
2. Reduce the image size using pngquant.
3. Resize the image to fit within the maximum allowed dimensions.

To run the script:
python3 optimize_cover_images.py --optimize

This script searches for *_cover.png images in ./resources/profiles/
"""

import os
import sys
import subprocess
import shutil
from pathlib import Path
from PIL import Image, ImageChops
import argparse


def get_file_size(path):
    """Get file size in bytes."""
    return os.path.getsize(path)


def format_size(size_bytes):
    """Format file size in human-readable format."""
    for unit in ['B', 'KB', 'MB']:
        if size_bytes < 1024.0:
            return f"{size_bytes:.1f} {unit}"
        size_bytes /= 1024.0
    return f"{size_bytes:.1f} GB"


def check_pngquant_available():
    """Check if pngquant is available in the system."""
    return shutil.which('pngquant') is not None


def optimize_png_with_pngquant(img_path, quality_range="65-80"):
    """
    Optimize PNG using pngquant for better compression.

    Args:
        img_path: Path to PNG file
        quality_range: Quality range for pngquant (e.g., "65-80")

    Returns:
        True if successful, False otherwise
    """
    try:
        # pngquant --quality 65-80 --force --ext .png image.png
        result = subprocess.run(
            ['pngquant', '--quality', quality_range,
                '--force', '--ext', '.png', str(img_path)],
            capture_output=True,
            text=True,
            timeout=30
        )
        return result.returncode == 0
    except Exception as e:
        print(f"    Warning: pngquant failed: {e}")
        return False


def optimize_png_pillow(img, output_path, has_transparency=True):
    """
    Optimize PNG using Pillow's best compression settings.

    Args:
        img: PIL Image object
        output_path: Path to save optimized image
        has_transparency: Whether image has transparency
    """
    # Use maximum compression
    # compress_level: 0-9, where 9 is maximum compression (slower but smaller)
    save_kwargs = {
        'format': 'PNG',
        'optimize': True,
        'compress_level': 9
    }

    # For images with transparency, ensure we're saving as RGBA
    if has_transparency and img.mode != 'RGBA':
        img = img.convert('RGBA')

    img.save(output_path, **save_kwargs)


def get_image_bbox(img):
    """
    Get the bounding box of non-transparent/non-white content in an image.

    Args:
        img: PIL Image object

    Returns:
        Tuple (left, top, right, bottom) or None if image is empty
    """
    # Convert to RGBA if not already
    if img.mode != 'RGBA':
        img = img.convert('RGBA')

    # Get the alpha channel
    alpha = img.split()[-1]

    # Find bounding box of non-transparent pixels
    bbox = alpha.getbbox()

    if bbox is None:
        # If all transparent, try to find non-white pixels in RGB
        if img.mode == 'RGBA':
            rgb = Image.new('RGB', img.size, (255, 255, 255))
            rgb.paste(img, mask=img.split()[-1])
            bg = Image.new('RGB', img.size, (255, 255, 255))
            diff = ImageChops.difference(rgb, bg)
            bbox = diff.getbbox()

    return bbox


def calculate_margins(bbox, img_size):
    """
    Calculate the current margins as a percentage of image size.

    Args:
        bbox: Tuple (left, top, right, bottom)
        img_size: Tuple (width, height)

    Returns:
        Dict with margin percentages
    """
    if bbox is None:
        return None

    left, top, right, bottom = bbox
    width, height = img_size

    content_width = right - left
    content_height = bottom - top

    margin_left = left / width * 100
    margin_top = top / height * 100
    margin_right = (width - right) / width * 100
    margin_bottom = (height - bottom) / height * 100

    content_width_pct = content_width / width * 100
    content_height_pct = content_height / height * 100

    return {
        'left': margin_left,
        'top': margin_top,
        'right': margin_right,
        'bottom': margin_bottom,
        'content_width': content_width_pct,
        'content_height': content_height_pct
    }


def adjust_image_margins(img_path, target_content_ratio=0.84, dry_run=False, use_pngquant=False, quality_range="65-80", max_size=None):
    """
    Adjust image so content takes up target_content_ratio of the image size.

    Args:
        img_path: Path to the image file
        target_content_ratio: Target ratio of content to image size (0.84 = 84%)
        dry_run: If True, don't save changes, just report
        use_pngquant: Use pngquant for additional compression
        quality_range: Quality range for pngquant
        max_size: Maximum dimension (width or height) in pixels, None to disable

    Returns:
        Dict with adjustment info or None if not adjusted
    """
    try:
        # Get original file size
        original_file_size = get_file_size(img_path)

        img = Image.open(img_path)
        original_size = img.size
        original_mode = img.mode

        # Convert to RGBA if the image has transparency
        has_transparency = original_mode in ('RGBA', 'LA') or (
            original_mode == 'P' and 'transparency' in img.info)
        if has_transparency and img.mode != 'RGBA':
            img = img.convert('RGBA')

        # Resize if image is too large
        was_resized = False
        if max_size and (img.size[0] > max_size or img.size[1] > max_size):
            # Calculate new size maintaining aspect ratio
            aspect_ratio = img.size[0] / img.size[1]
            if img.size[0] > img.size[1]:
                new_width = max_size
                new_height = int(max_size / aspect_ratio)
            else:
                new_height = max_size
                new_width = int(max_size * aspect_ratio)

            # Use high-quality resampling (LANCZOS for best quality)
            # Handle both old and new Pillow API
            try:
                resample = Image.Resampling.LANCZOS
            except AttributeError:
                resample = Image.LANCZOS

            img = img.resize((new_width, new_height), resample)
            was_resized = True

        # Get bounding box of actual content
        bbox = get_image_bbox(img)

        if bbox is None:
            print(f"  ⚠️  {img_path}: Image appears to be empty, skipping")
            return None

        left, top, right, bottom = bbox
        content_width = right - left
        content_height = bottom - top

        # Calculate current content ratio
        current_width_ratio = content_width / img.size[0]
        current_height_ratio = content_height / img.size[1]

        # Calculate margins
        margins = calculate_margins(bbox, img.size)

        print(f"\n📄 {img_path}")
        if was_resized:
            print(
                f"  Original Size: {original_size[0]}x{original_size[1]} → Resized to {img.size[0]}x{img.size[1]}")
        print(
            f"  Size: {img.size[0]}x{img.size[1]} (Mode: {original_mode}, Transparency: {has_transparency})")
        print(f"  File: {format_size(original_file_size)}")
        print(f"  Content: {content_width}x{content_height} " +
              f"({margins['content_width']:.1f}% x {margins['content_height']:.1f}%)")
        print(f"  Margins: L:{margins['left']:.1f}% T:{margins['top']:.1f}% " +
              f"R:{margins['right']:.1f}% B:{margins['bottom']:.1f}%")

        # Check if adjustment is needed (allow 5% tolerance)
        avg_ratio = (current_width_ratio + current_height_ratio) / 2
        tolerance = 0.05

        if abs(avg_ratio - target_content_ratio) < tolerance:
            print(f"  ✓ Already properly sized (avg ratio: {avg_ratio:.2f})")

            # If image was resized, we still need to save it
            if was_resized and not dry_run:
                optimize_png_pillow(img, img_path, has_transparency)
                new_file_size = get_file_size(img_path)

                if use_pngquant:
                    print(f"  🔧 Applying pngquant optimization...")
                    if optimize_png_with_pngquant(img_path, quality_range):
                        pngquant_size = get_file_size(img_path)
                        print(f"    pngquant: {format_size(new_file_size)} → {format_size(pngquant_size)} " +
                              f"({(pngquant_size/new_file_size-1)*100:+.1f}%)")
                        new_file_size = pngquant_size

                size_change_pct = (
                    new_file_size / original_file_size - 1) * 100
                print(f"  ✓ Saved (resized): {format_size(original_file_size)} → {format_size(new_file_size)} " +
                      f"({size_change_pct:+.1f}%)")

                return {
                    'adjusted': True,
                    'original_size': original_file_size,
                    'new_size': new_file_size,
                    'size_saved': original_file_size - new_file_size
                }

            return None

        # Crop to content
        cropped = img.crop(bbox)

        # Calculate new image size to achieve target ratio while preserving aspect ratio
        # We want: content_size / new_image_size = target_ratio
        # So: new_image_size = content_size / target_ratio
        # But we need to maintain the original aspect ratio

        original_aspect_ratio = img.size[0] / img.size[1]

        # Calculate required sizes for each dimension
        required_width = content_width / target_content_ratio
        required_height = content_height / target_content_ratio

        # Choose the larger requirement to ensure content fits within target ratio
        # Then adjust the other dimension to maintain aspect ratio
        if required_width / original_aspect_ratio > required_height:
            # Width is the limiting factor
            new_width = int(required_width)
            new_height = int(new_width / original_aspect_ratio)
        else:
            # Height is the limiting factor
            new_height = int(required_height)
            new_width = int(new_height * original_aspect_ratio)

        # Create new image with transparent/white background
        if has_transparency:
            new_img = Image.new(
                'RGBA', (new_width, new_height), (255, 255, 255, 0))
        else:
            new_img = Image.new(
                'RGB', (new_width, new_height), (255, 255, 255))

        # Calculate position to center the content
        paste_x = (new_width - content_width) // 2
        paste_y = (new_height - content_height) // 2

        # Paste cropped content onto new image
        if has_transparency:
            new_img.paste(cropped, (paste_x, paste_y), cropped)
        else:
            new_img.paste(cropped, (paste_x, paste_y))

        actual_content_ratio_w = content_width / new_width
        actual_content_ratio_h = content_height / new_height
        print(f"  → Adjusting to {new_width}x{new_height} " +
              f"(aspect ratio: {original_aspect_ratio:.2f}, " +
              f"content: {actual_content_ratio_w*100:.1f}% x {actual_content_ratio_h*100:.1f}%)")

        if not dry_run:
            # Save the adjusted image with optimization
            optimize_png_pillow(new_img, img_path, has_transparency)

            # Get new file size after Pillow optimization
            new_file_size = get_file_size(img_path)

            # Optionally use pngquant for additional compression
            if use_pngquant:
                print(f"  🔧 Applying pngquant optimization...")
                if optimize_png_with_pngquant(img_path, quality_range):
                    pngquant_size = get_file_size(img_path)
                    print(f"    pngquant: {format_size(new_file_size)} → {format_size(pngquant_size)} " +
                          f"({(pngquant_size/new_file_size-1)*100:+.1f}%)")
                    new_file_size = pngquant_size

            size_change_pct = (new_file_size / original_file_size - 1) * 100
            print(f"  ✓ Saved: {format_size(original_file_size)} → {format_size(new_file_size)} " +
                  f"({size_change_pct:+.1f}%)")

            return {
                'adjusted': True,
                'original_size': original_file_size,
                'new_size': new_file_size,
                'size_saved': original_file_size - new_file_size
            }
        else:
            print(f"  ⚠️  Dry run - not saved")
            return {
                'adjusted': False,
                'original_size': original_file_size,
                'new_size': original_file_size,
                'size_saved': 0
            }

    except Exception as e:
        print(f"  ❌ Error processing {img_path}: {e}")
        import traceback
        traceback.print_exc()
        return None


def find_and_process_cover_images(base_path, target_ratio=0.84, dry_run=False, use_pngquant=False, quality_range="65-80", max_size=None):
    """
    Find all *_cover.png images and process them.

    Args:
        base_path: Base directory to search
        target_ratio: Target content to image ratio
        dry_run: If True, don't save changes
        use_pngquant: Use pngquant for additional compression
        quality_range: Quality range for pngquant
        max_size: Maximum dimension (width or height) in pixels

    Returns:
        Dict with statistics
    """
    base_path = Path(base_path)

    if not base_path.exists():
        print(f"❌ Path does not exist: {base_path}")
        return {'total': 0, 'adjusted': 0, 'skipped': 0, 'errors': 0,
                'original_total_size': 0, 'new_total_size': 0, 'total_saved': 0}

    # Find all *_cover.png files
    cover_images = list(base_path.rglob('*_cover.png'))

    if not cover_images:
        print(f"⚠️  No *_cover.png files found in {base_path}")
        return {'total': 0, 'adjusted': 0, 'skipped': 0, 'errors': 0,
                'original_total_size': 0, 'new_total_size': 0, 'total_saved': 0}

    print(f"🔍 Found {len(cover_images)} cover image(s) in {base_path}")

    if use_pngquant:
        if check_pngquant_available():
            print(f"✓ pngquant is available and will be used")
        else:
            print(f"⚠️  pngquant not found in PATH, will use Pillow optimization only")
            print(
                f"   Install: brew install pngquant (macOS) or apt install pngquant (Linux)")
            use_pngquant = False

    stats = {
        'total': len(cover_images),
        'adjusted': 0,
        'skipped': 0,
        'errors': 0,
        'original_total_size': 0,
        'new_total_size': 0,
        'total_saved': 0
    }

    for img_path in cover_images:
        try:
            result = adjust_image_margins(
                img_path, target_ratio, dry_run, use_pngquant, quality_range, max_size)
            if result is None:
                stats['errors'] += 1
            elif result.get('adjusted'):
                stats['adjusted'] += 1
                stats['original_total_size'] += result['original_size']
                stats['new_total_size'] += result['new_size']
                stats['total_saved'] += result['size_saved']
            else:
                stats['skipped'] += 1
                stats['original_total_size'] += result['original_size']
                stats['new_total_size'] += result['original_size']
        except Exception as e:
            print(f"❌ Error processing {img_path}: {e}")
            stats['errors'] += 1

    return stats


def main():
    parser = argparse.ArgumentParser(
        description='Optimize cover images: \n'
                    '1. Scale the image to maintain proper margins around the content. \n'
                    '2. Reduce the image size using pngquant. \n'
                    '3. Resize the image to fit within the maximum allowed dimensions.',
        epilog='Examples:\n'
               '  %(prog)s --dry-run\n'
               '  %(prog)s --optimize\n'
               '  %(prog)s --optimize --quality 70-85\n'
               '  %(prog)s --vendor Custom\n'
               '  %(prog)s --vendor Custom --optimize\n'
               '  %(prog)s --max-size 200\n'
               '  %(prog)s --no-resize\n'
               '  %(prog)s --path ./custom/path --ratio 0.80\n'
               '\n'
               'Dependencies:\n'
               '  Required: pip3 install Pillow\n'
               '  Optional (for --optimize):\n'
               '    macOS:  brew install pngquant\n'
               '    Linux:  sudo apt install pngquant\n'
               '    Arch:   sudo pacman -S pngquant\n'
               '    Windows: choco install pngquant or download from https://pngquant.org/',
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        '--path',
        default='./resources/profiles',
        help='Base path to search for cover images (default: ./resources/profiles)'
    )
    parser.add_argument(
        '--vendor',
        type=str,
        help='Process only a specific vendor subfolder (e.g., "Custom")'
    )
    parser.add_argument(
        '--ratio',
        type=float,
        default=0.64,
        help='Target content to image ratio (default: 0.64 = 64%%)'
    )
    parser.add_argument(
        '--dry-run',
        action='store_true',
        help='Preview changes without saving'
    )
    parser.add_argument(
        '--optimize',
        action='store_true',
        help='Use pngquant for additional compression (must be installed)'
    )
    parser.add_argument(
        '--quality',
        default='65-80',
        help='Quality range for pngquant (default: 65-80). Lower = smaller files'
    )
    parser.add_argument(
        '--max-size',
        type=int,
        default=240,
        help='Maximum image dimension in pixels (default: 240). Images larger than this will be resized'
    )
    parser.add_argument(
        '--no-resize',
        action='store_true',
        help='Disable automatic resizing of large images'
    )

    args = parser.parse_args()

    print("=" * 70)
    print("Cover Image Margin Adjuster & Optimizer")
    print("=" * 70)

    if args.dry_run:
        print("⚠️  DRY RUN MODE - No changes will be saved\n")

    # Determine the search path
    search_path = args.path
    if args.vendor:
        search_path = os.path.join(args.path, args.vendor)
        print(f"🎯 Processing vendor: {args.vendor}")
        print(f"   Path: {search_path}")
        
        # Check if vendor path exists
        if not os.path.exists(search_path):
            print(f"❌ Error: Vendor path does not exist: {search_path}")
            print(f"\nAvailable vendors in {args.path}:")
            try:
                vendors = [d for d in os.listdir(args.path) 
                          if os.path.isdir(os.path.join(args.path, d)) and not d.startswith('.')]
                for vendor in sorted(vendors):
                    print(f"  - {vendor}")
            except Exception:
                pass
            return 1
        print()

    # Determine max size (None if --no-resize is specified)
    max_size = None if args.no_resize else args.max_size

    if max_size:
        print(f"📏 Images will be resized to max {max_size}px if larger\n")

    stats = find_and_process_cover_images(
        search_path,
        args.ratio,
        args.dry_run,
        args.optimize,
        args.quality,
        max_size
    )

    print("\n" + "=" * 70)
    print("Summary:")
    print(f"  Total images: {stats['total']}")
    print(f"  Adjusted: {stats['adjusted']}")
    print(f"  Already correct: {stats['skipped']}")
    print(f"  Errors: {stats['errors']}")

    if stats['adjusted'] > 0:
        print(f"\n  File Size:")
        print(f"  Original: {format_size(stats['original_total_size'])}")
        print(f"  New: {format_size(stats['new_total_size'])}")
        if stats['total_saved'] > 0:
            saved_pct = (stats['total_saved'] /
                         stats['original_total_size']) * 100
            print(
                f"  Saved: {format_size(stats['total_saved'])} ({saved_pct:.1f}%)")
        elif stats['total_saved'] < 0:
            increased_pct = (-stats['total_saved'] /
                             stats['original_total_size']) * 100
            print(
                f"  Increased: {format_size(-stats['total_saved'])} (+{increased_pct:.1f}%)")

    print("=" * 70)

    return 0 if stats['errors'] == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
