#!/usr/bin/env python2.7
# -*- coding: utf-8 -*-
"""
Digital Picture Frame Bitmap Browser
Browses and displays bitmap files from ct952-dmp-121 project
Compatible with Python 2.7
"""

import os
import sys
import re
import struct
import math
from PIL import Image, ImageTk
import Tkinter as tk
import tkFileDialog
import tkMessageBox
from itertools import product

class BitmapBrowser(object):
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("Digital Picture Frame Bitmap Browser")
        self.root.geometry("800x600")
        
        self.current_data = None
        self.current_filename = ""
        self.image_label = None
        
        self.setup_ui()
        
    def setup_ui(self):
        """Setup the user interface"""
        # Main frame
        main_frame = tk.Frame(self.root)
        main_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        
        # Control panel
        control_frame = tk.Frame(main_frame)
        control_frame.pack(fill=tk.X, pady=(0, 10))
        
        # File selection
        tk.Button(control_frame, text="Open Bitmap File", 
                 command=self.open_file).pack(side=tk.LEFT, padx=(0, 5))
        
        self.filename_label = tk.Label(control_frame, text="No file selected")
        self.filename_label.pack(side=tk.LEFT, padx=(5, 0))
        
        # Format controls
        format_frame = tk.Frame(main_frame)
        format_frame.pack(fill=tk.X, pady=(0, 10))
        
        tk.Label(format_frame, text="Width:").pack(side=tk.LEFT)
        self.width_var = tk.StringVar(value="320")
        tk.Entry(format_frame, textvariable=self.width_var, width=8).pack(side=tk.LEFT, padx=(5, 10))
        
        tk.Label(format_frame, text="Height:").pack(side=tk.LEFT)
        self.height_var = tk.StringVar(value="240")
        tk.Entry(format_frame, textvariable=self.height_var, width=8).pack(side=tk.LEFT, padx=(5, 10))
        
        tk.Label(format_frame, text="Format:").pack(side=tk.LEFT)
        self.format_var = tk.StringVar(value="8-bit grayscale")
        format_menu = tk.OptionMenu(format_frame, self.format_var, 
                                   "1-bit monochrome", "8-bit grayscale", 
                                   "16-bit RGB565", "24-bit RGB")
        format_menu.pack(side=tk.LEFT, padx=(5, 10))
        
        tk.Button(format_frame, text="Render", 
                 command=self.render_bitmap).pack(side=tk.LEFT, padx=(10, 0))
        
        tk.Button(format_frame, text="Auto-detect", 
                 command=self.auto_detect).pack(side=tk.LEFT, padx=(5, 0))
        
        # Image display area
        self.image_frame = tk.Frame(main_frame, bg="white", relief=tk.SUNKEN, bd=2)
        self.image_frame.pack(fill=tk.BOTH, expand=True)
        
        # Status bar
        self.status_var = tk.StringVar(value="Ready")
        status_bar = tk.Label(main_frame, textvariable=self.status_var, 
                             relief=tk.SUNKEN, anchor=tk.W)
        status_bar.pack(fill=tk.X, pady=(10, 0))
        
    def open_file(self):
        """Open and parse a bitmap file"""
        filename = tkFileDialog.askopenfilename(
            title="Select bitmap file",
            filetypes=[("Text files", "*.txt"), ("All files", "*.*")]
        )
        
        if filename:
            try:
                self.current_data = self.parse_hex_file(filename)
                self.current_filename = os.path.basename(filename)
                self.filename_label.config(text=self.current_filename)
                self.status_var.set("Loaded {} bytes from {}".format(
                    len(self.current_data), self.current_filename))
                
                # Try to auto-detect format
                self.auto_detect()
                
            except Exception as e:
                tkMessageBox.showerror("Error", "Failed to load file: {}".format(str(e)))
                
    def parse_hex_file(self, filename):
        """Parse hexadecimal data from file"""
        with open(filename, 'r') as f:
            content = f.read()
        
        # Extract hex values using regex
        hex_pattern = r'0x([0-9A-Fa-f]{2})'
        hex_matches = re.findall(hex_pattern, content)
        
        if not hex_matches:
            raise ValueError("No hexadecimal data found in file")
        
        # Convert to bytes
        data = bytearray()
        for hex_str in hex_matches:
            data.append(int(hex_str, 16))
            
        return data
    
    def auto_detect(self):
        """Attempt to auto-detect bitmap dimensions and format"""
        if not self.current_data:
            return
            
        data_len = len(self.current_data)
        self.status_var.set("Auto-detecting format for {} bytes...".format(data_len))
        
        # Common digital picture frame resolutions
        common_resolutions = [
            (320, 240), (480, 272), (800, 480), (1024, 600),
            (640, 480), (480, 320), (272, 480), (240, 320)
        ]
        
        # Try different bit depths
        bit_depths = [1, 8, 16, 24]
        
        best_match = None
        best_score = float('inf')
        
        for (width, height), bits in product(common_resolutions, bit_depths):
            expected_bytes = (width * height * bits) // 8
            score = abs(expected_bytes - data_len)
            
            if score < best_score:
                best_score = score
                best_match = (width, height, bits)
        
        if best_match:
            width, height, bits = best_match
            self.width_var.set(str(width))
            self.height_var.set(str(height))
            
            if bits == 1:
                self.format_var.set("1-bit monochrome")
            elif bits == 8:
                self.format_var.set("8-bit grayscale")
            elif bits == 16:
                self.format_var.set("16-bit RGB565")
            else:
                self.format_var.set("24-bit RGB")
                
            self.status_var.set("Auto-detected: {}x{} {}-bit (score: {})".format(
                width, height, bits, best_score))
            
            # Auto-render if it's a close match
            if best_score < data_len * 0.1:  # Within 10%
                self.render_bitmap()
    
    def render_bitmap(self):
        """Render the bitmap with current settings"""
        if not self.current_data:
            tkMessageBox.showwarning("Warning", "No data loaded")
            return
            
        try:
            width = int(self.width_var.get())
            height = int(self.height_var.get())
            format_type = self.format_var.get()
            
            if format_type == "1-bit monochrome":
                image = self.render_1bit(width, height)
            elif format_type == "8-bit grayscale":
                image = self.render_8bit_gray(width, height)
            elif format_type == "16-bit RGB565":
                image = self.render_16bit_rgb565(width, height)
            elif format_type == "24-bit RGB":
                image = self.render_24bit_rgb(width, height)
            else:
                raise ValueError("Unknown format: {}".format(format_type))
            
            self.display_image(image)
            self.status_var.set("Rendered {}x{} {} image".format(width, height, format_type))
            
        except Exception as e:
            tkMessageBox.showerror("Render Error", str(e))
            
    def render_1bit(self, width, height):
        """Render 1-bit monochrome bitmap"""
        expected_bytes = (width * height + 7) // 8
        if len(self.current_data) < expected_bytes:
            raise ValueError("Not enough data for {}x{} 1-bit image".format(width, height))
        
        image = Image.new('L', (width, height))
        pixels = []
        
        bit_index = 0
        for y in range(height):
            for x in range(width):
                byte_index = bit_index // 8
                bit_offset = bit_index % 8
                
                if byte_index < len(self.current_data):
                    byte_val = self.current_data[byte_index]
                    bit_val = (byte_val >> (7 - bit_offset)) & 1
                    pixels.append(255 if bit_val else 0)
                else:
                    pixels.append(0)
                    
                bit_index += 1
        
        image.putdata(pixels)
        return image
    
    def render_8bit_gray(self, width, height):
        """Render 8-bit grayscale bitmap"""
        expected_bytes = width * height
        if len(self.current_data) < expected_bytes:
            raise ValueError("Not enough data for {}x{} 8-bit image".format(width, height))
        
        image = Image.new('L', (width, height))
        image.putdata(self.current_data[:expected_bytes])
        return image
    
    def render_16bit_rgb565(self, width, height):
        """Render 16-bit RGB565 bitmap"""
        expected_bytes = width * height * 2
        if len(self.current_data) < expected_bytes:
            raise ValueError("Not enough data for {}x{} 16-bit image".format(width, height))
        
        image = Image.new('RGB', (width, height))
        pixels = []
        
        for i in range(0, min(expected_bytes, len(self.current_data)), 2):
            if i + 1 < len(self.current_data):
                # Little endian RGB565
                low_byte = self.current_data[i]
                high_byte = self.current_data[i + 1]
                rgb565 = (high_byte << 8) | low_byte
                
                # Extract RGB components
                r = ((rgb565 >> 11) & 0x1F) << 3
                g = ((rgb565 >> 5) & 0x3F) << 2
                b = (rgb565 & 0x1F) << 3
                
                pixels.append((r, g, b))
            else:
                pixels.append((0, 0, 0))
        
        image.putdata(pixels)
        return image
    
    def render_24bit_rgb(self, width, height):
        """Render 24-bit RGB bitmap"""
        expected_bytes = width * height * 3
        if len(self.current_data) < expected_bytes:
            raise ValueError("Not enough data for {}x{} 24-bit image".format(width, height))
        
        image = Image.new('RGB', (width, height))
        pixels = []
        
        for i in range(0, min(expected_bytes, len(self.current_data)), 3):
            if i + 2 < len(self.current_data):
                r = self.current_data[i]
                g = self.current_data[i + 1]
                b = self.current_data[i + 2]
                pixels.append((r, g, b))
            else:
                pixels.append((0, 0, 0))
        
        image.putdata(pixels)
        return image
    
    def display_image(self, image):
        """Display the rendered image"""
        # Clear previous image
        for widget in self.image_frame.winfo_children():
            widget.destroy()
        
        # Scale image to fit display area if needed
        display_image = image.copy()
        max_width = 600
        max_height = 400
        
        if image.width > max_width or image.height > max_height:
            ratio = min(max_width / float(image.width), max_height / float(image.height))
            new_size = (int(image.width * ratio), int(image.height * ratio))
            display_image = image.resize(new_size, Image.NEAREST)
        
        # Convert to PhotoImage and display
        photo = ImageTk.PhotoImage(display_image)
        self.image_label = tk.Label(self.image_frame, image=photo)
        self.image_label.image = photo  # Keep a reference
        self.image_label.pack(expand=True)
        
        # Add save button
        save_button = tk.Button(self.image_frame, text="Save Image", 
                               command=lambda: self.save_image(image))
        save_button.pack(pady=5)
    
    def save_image(self, image):
        """Save the rendered image"""
        filename = tkFileDialog.asksaveasfilename(
            title="Save image",
            defaultextension=".png",
            filetypes=[("PNG files", "*.png"), ("JPEG files", "*.jpg"), ("All files", "*.*")]
        )
        
        if filename:
            try:
                image.save(filename)
                tkMessageBox.showinfo("Success", "Image saved successfully")
            except Exception as e:
                tkMessageBox.showerror("Error", "Failed to save image: {}".format(str(e)))
    
    def run(self):
        """Start the application"""
        self.root.mainloop()

def main():
    """Main entry point"""
    if len(sys.argv) > 1:
        print("Digital Picture Frame Bitmap Browser")
        print("Usage: python {} [bitmap_file.txt]".format(sys.argv[0]))
        print("\nThis tool helps visualize bitmap data from the ct952-dmp-121 project.")
        print("It supports multiple formats commonly used in embedded systems:")
        print("- 1-bit monochrome")
        print("- 8-bit grayscale") 
        print("- 16-bit RGB565")
        print("- 24-bit RGB")
        print("\nRequires: PIL/Pillow, Tkinter")
        return
    
    try:
        app = BitmapBrowser()
        app.run()
    except ImportError as e:
        print("Error: Missing required library")
        print("Please install: pip install Pillow")
        print("Tkinter should be included with Python 2.7")
        sys.exit(1)
    except Exception as e:
        print("Error starting application: {}".format(str(e)))
        sys.exit(1)

if __name__ == "__main__":
    main()