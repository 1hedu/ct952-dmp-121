from PIL import Image, ImageDraw

palette_hex = [
    0x00000019, 0x00108080, 0x00e98080, 0x0069cadd, 0x0047adb9, 0x00d18e91,
    0x00628181, 0x0028ef6e, 0x001fc475, 0x00c4957c, 0x00a8a511,
    0x006e973b, 0x00dc876b, 0x00903623, 0x005f5346, 0x00d8726f,
    0x00c08080, 0x00873c8a, 0x00d11192, 0x00e56b84, 0x00b91fa3,
    0x00813fcc, 0x00505cb3, 0x00515bef, 0x003869c4, 0x00cd7995,
]

def hex_to_yuv_rgb(hex_value):
    """
    Interprets 0x00RRGGBB as 0x00YUVV and converts to RGB for display.
    Assumes R=Y, G=U, B=V. Uses BT.601 full range conversion.
    """
    # Extract components assuming 0x00YUVV packing
    Y = (hex_value >> 16) & 0xFF
    U = (hex_value >> 8) & 0xFF
    V = hex_value & 0xFF

    # Adjust U and V for conversion (typically centered around 128)
    _U = U - 128
    _V = V - 128

    # Convert YUV to RGB using standard BT.601 full-range formulas
    R = Y + 1.402 * _V
    G = Y - 0.344136 * _U - 0.714136 * _V
    B = Y + 1.772 * _U

    # Clamp RGB values to the valid 0-255 range
    R = max(0, min(255, int(R)))
    G = max(0, min(255, int(G)))
    B = max(0, min(255, int(B)))

    return (R, G, B)

# Convert all hex values in the palette to their YUV-interpreted RGB equivalents
colors = [hex_to_yuv_rgb(h) for h in palette_hex]

# Create an image to visualize the palette
num_colors = len(colors)
color_width = 50
image_width = num_colors * color_width
image_height = color_width

img = Image.new('RGB', (image_width, image_height))
draw = ImageDraw.Draw(img)

# Draw each color as a rectangle in the image
for i, color in enumerate(colors):
    x0 = i * color_width
    y0 = 0
    x1 = (i + 1) * color_width
    y1 = color_width
    draw.rectangle([x0, y0, x1, y1], fill=color)

# Save the generated image
img.save("palette_visualization_yuv.png")
print("Palette visualization (YUV interpretation) saved as palette_visualization_yuv.png")

# Print the original hex, interpreted YUV components, and resulting RGB for reference
print("\nRGB Values (assuming 0x00YUVV input and conversion to display RGB):")
for i, hex_val in enumerate(palette_hex):
    Y_val = (hex_val >> 16) & 0xFF
    U_val = (hex_val >> 8) & 0xFF
    V_val = hex_val & 0xFF
    r, g, b = hex_to_yuv_rgb(hex_val)
    print("Index {0:2d}: 0x{1:08X} -> YUV Input: ({2:3d}, {3:3d}, {4:3d}) -> Display RGB({5:3d}, {6:3d}, {7:3d})".format(
        i, hex_val, Y_val, U_val, V_val, r, g, b
    ))