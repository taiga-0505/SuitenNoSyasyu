import os
from PIL import Image, ImageDraw

output_dir = r"c:\Users\rea\source\repos\2025\Chaso\Chaso\project\Resources\icons"
os.makedirs(output_dir, exist_ok=True)

size = 64
center = size // 2
radius = 24
box = (center - radius, center - radius, center + radius, center + radius)

def create_image():
    return Image.new("RGBA", (size, size), (0, 0, 0, 0))

# 1. Solid: filled circle
img = create_image()
draw = ImageDraw.Draw(img)
draw.ellipse(box, fill=(255, 255, 255, 255))
img.save(os.path.join(output_dir, "solid.png"))

# 2. Wireframe: outline only with grid lines
img = create_image()
draw = ImageDraw.Draw(img)
# outer circle
draw.ellipse(box, outline=(255, 255, 255, 255), width=3)
# grid lines
draw.line((center, center - radius, center, center + radius), fill=(255, 255, 255, 200), width=2)
draw.line((center - radius, center, center + radius, center), fill=(255, 255, 255, 200), width=2)
# longitude/latitude curves
draw.arc((center - radius//2, center - radius, center + radius//2, center + radius), 0, 360, fill=(255,255,255,200), width=2)
draw.arc((center - radius, center - radius//2, center + radius, center + radius//2), 0, 360, fill=(255,255,255,200), width=2)
img.save(os.path.join(output_dir, "wireframe.png"))

# 3. Solid + Wireframe: solid circle with dark grid
img = create_image()
draw = ImageDraw.Draw(img)
draw.ellipse(box, fill=(255, 255, 255, 255))
draw.line((center, center - radius, center, center + radius), fill=(100, 100, 100, 255), width=2)
draw.line((center - radius, center, center + radius, center), fill=(100, 100, 100, 255), width=2)
draw.arc((center - radius//2, center - radius, center + radius//2, center + radius), 0, 360, fill=(100, 100, 100, 255), width=2)
draw.arc((center - radius, center - radius//2, center + radius, center + radius//2), 0, 360, fill=(100, 100, 100, 255), width=2)
img.save(os.path.join(output_dir, "solid_wire.png"))

# 4. Face Orientation: half colored
img = create_image()
draw = ImageDraw.Draw(img)
draw.pieslice(box, 0, 180, fill=(100, 100, 255, 255))  # Blue (front)
draw.pieslice(box, 180, 360, fill=(255, 100, 100, 255)) # Red (back)
img.save(os.path.join(output_dir, "face.png"))

# 5. Random Color: 4 colored quadrants
img = create_image()
draw = ImageDraw.Draw(img)
draw.pieslice(box, 0, 90, fill=(200, 255, 200, 255))
draw.pieslice(box, 90, 180, fill=(255, 200, 200, 255))
draw.pieslice(box, 180, 270, fill=(200, 200, 255, 255))
draw.pieslice(box, 270, 360, fill=(255, 255, 200, 255))
img.save(os.path.join(output_dir, "random.png"))

# 6. Lambert: Shaded sphere
img = create_image()
# Generate a radial gradient manually
for y in range(size):
    for x in range(size):
        dx = x - center
        dy = y - center
        r = (dx*dx + dy*dy)**0.5
        if r <= radius:
            # simple lambertian-like lighting (light from top-left)
            lx = -0.5
            ly = -0.5
            lz = 1.0
            ln = (lx*lx + ly*ly + lz*lz)**0.5
            lx/=ln; ly/=ln; lz/=ln
            
            nz = max(0.0, radius*radius - dx*dx - dy*dy)**0.5
            nx = dx; ny = dy
            nn = (nx*nx + ny*ny + nz*nz)**0.5
            nx/=nn; ny/=nn; nz/=nn
            
            dot = max(0.0, nx*lx + ny*ly + nz*lz)
            intensity = int(50 + 205 * dot)
            img.putpixel((x, y), (intensity, intensity, intensity, 255))

# Anti-alias edges simply
draw = ImageDraw.Draw(img)
draw.ellipse(box, outline=(255, 255, 255, 100), width=1)
img.save(os.path.join(output_dir, "lambert.png"))

print("Icons generated.")
