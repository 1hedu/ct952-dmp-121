from PIL import Image, ImageDraw

palette_hex = [
    0x00000019, 0x00108080, 0x00e98080, 0x0069cadd, 0x0047adb9, 0x00d18e91,
    0x00628181, 0x0028ef6e, 0x001fc475, 0x00c4957c, 0x00a8a511,
    0x006e973b, 0x00dc876b, 0x00903623, 0x005f5346, 0x00d8726f,
    0x00c08080, 0x00873c8a, 0x00d11192, 0x00e56b84, 0x00b91fa3,
    0x00813fcc, 0x00505cb3, 0x00515bef, 0x003869c4, 0x00cd7995,
]

# Assuming 0x00RRGGBB format
def hex_to_rgb(hex_value):
    r = (hex_value >> 16) & 0xFF
    g = (hex_value >> 8) & 0xFF
    b = hex_value & 0xFF
    return (r, g, b)

colors = [hex_to_rgb(h) for h in palette_hex]

# Create an image to display the palette
num_colors = len(colors)
color_width = 50
image_width = num_colors * color_width
image_height = color_width # A single row for simplicity

img = Image.new('RGB', (image_width, image_height))
draw = ImageDraw.Draw(img)

for i, color in enumerate(colors):
    x0 = i * color_width
    y0 = 0
    x1 = (i + 1) * color_width
    y1 = color_width
    draw.rectangle([x0, y0, x1, y1], fill=color)

img.save("palette_visualization.png")
print("Palette visualization saved as palette_visualization.png")

# Also print RGB values for reference
print("\nRGB Values:")
for i, hex_val in enumerate(palette_hex):
    r, g, b = hex_to_rgb(hex_val)
    # Corrected line using .format()
    print("Index {0:2d}: 0x{1:08X} -> RGB({2:3d}, {3:3d}, {4:3d})".format(i, hex_val, r, g, b))