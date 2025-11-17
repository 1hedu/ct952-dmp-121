# -*- coding: utf-8 -*-
"""
Enhanced CT952 Digital Picture Frame Sprite/Icon Browser
Fixed version with debugging features
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

class SpriteBrowser(object):
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("Enhanced CT952 Sprite/Icon Browser")
        self.root.geometry("900x700")
        
        self.current_data = None
        self.current_filename = ""
        self.current_sprites = []
        self.current_palette = None 
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
        tk.Button(control_frame, text="Open Sprite File", 
                  command=self.open_file).pack(side=tk.LEFT, padx=(0, 5))

        tk.Button(control_frame, text="Load Palette File",
                  command=self.load_palette_file).pack(side=tk.LEFT, padx=(0, 5))
        
        self.filename_label = tk.Label(control_frame, text="No file selected")
        self.filename_label.pack(side=tk.LEFT, padx=(5, 0))
        
        # Palette info
        palette_info_frame = tk.Frame(main_frame)
        palette_info_frame.pack(fill=tk.X, pady=(0, 5))
        
        self.palette_label = tk.Label(palette_info_frame, text="No palette loaded", fg="red")
        self.palette_label.pack(side=tk.LEFT)
        
        tk.Button(palette_info_frame, text="Browse Palettes", 
                  command=self.browse_palettes).pack(side=tk.RIGHT)
        
        # Auto-analysis button
        analysis_frame = tk.Frame(main_frame)
        analysis_frame.pack(fill=tk.X, pady=(0, 10))
        
        tk.Button(analysis_frame, text="Auto-Analyze Format", 
                  command=self.auto_analyze_format, bg="lightblue").pack(side=tk.LEFT)
        
        self.analysis_label = tk.Label(analysis_frame, text="Click Auto-Analyze to detect format", fg="blue")
        self.analysis_label.pack(side=tk.LEFT, padx=(10, 0))
        
        # Sprite info and controls
        info_frame = tk.Frame(main_frame)
        info_frame.pack(fill=tk.X, pady=(0, 10))
        
        self.info_label = tk.Label(info_frame, text="Sprite Info: -")
        self.info_label.pack(side=tk.LEFT)
        
        # Format controls
        format_frame = tk.Frame(main_frame)
        format_frame.pack(fill=tk.X, pady=(0, 10))
        
        tk.Label(format_frame, text="Format:").pack(side=tk.LEFT)
        self.format_var = tk.StringVar(value="8bpp_indexed")
        format_menu = tk.OptionMenu(format_frame, self.format_var, 
                                   "8bpp_indexed", "4bpp_indexed", "2bpp_indexed", "1bpp_mono")
        format_menu.pack(side=tk.LEFT, padx=(5, 10))
        
        tk.Label(format_frame, text="Scale:").pack(side=tk.LEFT)
        self.scale_var = tk.StringVar(value="8")
        scale_menu = tk.OptionMenu(format_frame, self.scale_var, "1", "2", "4", "8", "16")
        scale_menu.pack(side=tk.LEFT, padx=(5, 10))
        
        tk.Button(format_frame, text="Render", 
                  command=self.render_sprites).pack(side=tk.LEFT, padx=(10, 0))
        
        # Additional format options
        options_frame = tk.Frame(main_frame)
        options_frame.pack(fill=tk.X, pady=(0, 10))
        
        # Color inversion options
        self.invert_colors_var = tk.BooleanVar()
        tk.Checkbutton(options_frame, text="Invert Colors", 
                      variable=self.invert_colors_var).pack(side=tk.LEFT, padx=(0, 10))
        
        self.invert_palette_var = tk.BooleanVar()
        tk.Checkbutton(options_frame, text="Invert Palette", 
                      variable=self.invert_palette_var).pack(side=tk.LEFT, padx=(0, 10))
        
        # Endianness
        self.little_endian_var = tk.BooleanVar()
        tk.Checkbutton(options_frame, text="Little Endian", 
                      variable=self.little_endian_var).pack(side=tk.LEFT, padx=(0, 10))
        
        # Palette debugging options
        palette_debug_frame = tk.Frame(main_frame)
        palette_debug_frame.pack(fill=tk.X, pady=(0, 10))
        
        tk.Label(palette_debug_frame, text="Palette Debug:").pack(side=tk.LEFT)
        
        tk.Label(palette_debug_frame, text="Offset:").pack(side=tk.LEFT, padx=(10, 0))
        self.palette_offset_var = tk.StringVar(value="0")
        palette_offset_entry = tk.Entry(palette_debug_frame, textvariable=self.palette_offset_var, width=5)
        palette_offset_entry.pack(side=tk.LEFT, padx=(5, 10))
        
        tk.Label(palette_debug_frame, text="Count:").pack(side=tk.LEFT)
        self.palette_count_var = tk.StringVar(value="256")
        palette_count_entry = tk.Entry(palette_debug_frame, textvariable=self.palette_count_var, width=5)
        palette_count_entry.pack(side=tk.LEFT, padx=(5, 10))
        
        # Brightness mapping controls
        brightness_frame = tk.Frame(main_frame)
        brightness_frame.pack(fill=tk.X, pady=(0, 10))
        
        tk.Label(brightness_frame, text="Brightness Mapping:").pack(side=tk.LEFT)
        
        tk.Label(brightness_frame, text="Shift Right:").pack(side=tk.LEFT, padx=(10, 0))
        self.brightness_shift_var = tk.StringVar(value="0")
        brightness_shift_entry = tk.Entry(brightness_frame, textvariable=self.brightness_shift_var, width=3)
        brightness_shift_entry.pack(side=tk.LEFT, padx=(5, 5))
        
        tk.Label(brightness_frame, text="Subtract:").pack(side=tk.LEFT, padx=(5, 0))
        self.brightness_subtract_var = tk.StringVar(value="0")
        brightness_subtract_entry = tk.Entry(brightness_frame, textvariable=self.brightness_subtract_var, width=4)
        brightness_subtract_entry.pack(side=tk.LEFT, padx=(5, 5))
        
        tk.Label(brightness_frame, text="Scale:").pack(side=tk.LEFT, padx=(5, 0))
        self.brightness_scale_var = tk.StringVar(value="1.0")
        brightness_scale_entry = tk.Entry(brightness_frame, textvariable=self.brightness_scale_var, width=4)
        brightness_scale_entry.pack(side=tk.LEFT, padx=(5, 10))
        
        tk.Button(brightness_frame, text="Quick Test: 95→0", 
                  command=self.quick_test_95_to_0).pack(side=tk.LEFT, padx=(10, 5))
        
        tk.Button(brightness_frame, text="Quick Test: /4", 
                  command=self.quick_test_divide_4).pack(side=tk.LEFT, padx=(5, 5))
        
        tk.Button(palette_debug_frame, text="Show Palette Info", 
                  command=self.show_palette_debug).pack(side=tk.LEFT, padx=(10, 0))
        
        tk.Button(palette_debug_frame, text="Show Sprite Data", 
                  command=self.show_sprite_debug).pack(side=tk.LEFT, padx=(5, 0))
        
        # Sprite selection
        sprite_frame = tk.Frame(main_frame)
        sprite_frame.pack(fill=tk.X, pady=(0, 10))
        
        tk.Label(sprite_frame, text="Interpretation:").pack(side=tk.LEFT)
        self.sprite_var = tk.StringVar(value="0")
        self.sprite_menu = tk.OptionMenu(sprite_frame, self.sprite_var, "0")
        self.sprite_menu.pack(side=tk.LEFT, padx=(5, 0))
        
        # Image display area
        self.image_frame = tk.Frame(main_frame, bg="white", relief=tk.SUNKEN, bd=2)
        self.image_frame.pack(fill=tk.BOTH, expand=True)
        
        # Status bar
        self.status_var = tk.StringVar(value="Ready")
        status_bar = tk.Label(main_frame, textvariable=self.status_var, 
                               relief=tk.SUNKEN, anchor=tk.W)
        status_bar.pack(fill=tk.X, pady=(10, 0))
        
    def open_file(self):
        """Open and parse a sprite file"""
        filename = tkFileDialog.askopenfilename(
            title="Select sprite/data file", 
            filetypes=[("Data files", "*.txt *.bin"), ("All files", "*.*")]
        )
        
        if filename:
            try:
                self.current_data = self.parse_image_data_from_file(filename)
                self.current_filename = os.path.basename(filename)
                self.current_file_path = filename 
                
                self.filename_label.config(text=self.current_filename)
                
                # Check if it's a known palette file
                if "pal" in self.current_filename.lower():
                    self.load_palette_from_data(self.current_data, self.current_filename)
                    self.palette_label.config(text="Palette: {}".format(self.current_filename), fg="green")
                    self.status_var.set("Loaded palette (YUV interpreted) from {}".format(self.current_filename))
                    self.current_sprites = [] 
                    self.analysis_label.config(text="Palette file - no sprite analysis needed")
                else:
                    self.current_sprites = self.parse_sprites(self.current_data)
                    self.try_auto_load_palette(filename)
                    self.update_sprite_menu()
                    self.render_sprites()
                    self.analysis_label.config(text="Click Auto-Analyze to detect best format", fg="blue")
                
            except Exception as e:
                tkMessageBox.showerror("Error", "Failed to load file: {}".format(str(e)))
                
    def parse_image_data_from_file(self, filename):
        """Parse the file to extract 32-bit values (hex from .txt, raw from .bin)"""
        data = []
        
        if filename.lower().endswith('.txt'):
            with open(filename, 'r') as f:
                content = f.read()
            
            hex_pattern = r'0x([0-9A-Fa-f]{8})'
            hex_matches = re.findall(hex_pattern, content)
            
            if not hex_matches:
                raise ValueError("No 32-bit hexadecimal data found in .txt file")
            
            for hex_str in hex_matches:
                data.append(int(hex_str, 16))
                
        elif filename.lower().endswith('.bin'):
            with open(filename, 'rb') as f:
                binary_content = f.read()
            
            if len(binary_content) % 4 != 0:
                raise ValueError("Binary file size is not a multiple of 4 bytes (expected 32-bit values)")
            
            format_string = '>I' 
            
            for i in range(0, len(binary_content), 4):
                word = struct.unpack(format_string, binary_content[i:i+4])[0]
                data.append(word)

        else:
            raise ValueError("Unsupported file type. Please select a .txt (hex dump) or .bin file.")
            
        return data

    def auto_analyze_format(self):
        """Automatically analyze the data to suggest the best format"""
        if not self.current_data:
            tkMessageBox.showwarning("Warning", "Please load a sprite file first")
            return
            
        analysis_results = []
        
        # Check for header format (magic, version, width, height)
        if len(self.current_data) >= 4:
            potential_width = self.current_data[2]
            potential_height = self.current_data[3]
            
            if (potential_width > 0 and potential_height > 0 and 
                potential_width <= 512 and potential_height <= 512):
                
                expected_pixels = potential_width * potential_height
                available_data = len(self.current_data) - 4
                
                # Check various packing schemes
                if available_data >= expected_pixels // 4:
                    analysis_results.append("LIKELY: Header + 4-pixels/word ({}x{})".format(potential_width, potential_height))
                if available_data >= expected_pixels // 8:
                    analysis_results.append("POSSIBLE: Header + 8-pixels/word ({}x{})".format(potential_width, potential_height))
        
        # Check for raw data interpretation with common sizes
        data_length = len(self.current_data)
        common_sizes = [
            (8, 8), (16, 16), (24, 24), (32, 32), (48, 48), (64, 64),
            (16, 8), (32, 16), (64, 32), (48, 40), (80, 60), (96, 64)
        ]
        
        for width, height in common_sizes:
            pixels = width * height
            if data_length == pixels // 4:
                analysis_results.append("POSSIBLE: Raw 4-pixels/word ({}x{})".format(width, height))
            elif data_length == pixels // 8:
                analysis_results.append("POSSIBLE: Raw 8-pixels/word ({}x{})".format(width, height))
        
        # Data pattern analysis
        unique_values = len(set(self.current_data))
        max_value = max(self.current_data) if self.current_data else 0
        
        analysis_results.append("--- Data Analysis ---")
        analysis_results.append("Total words: {}".format(len(self.current_data)))
        analysis_results.append("Unique values: {}".format(unique_values))
        analysis_results.append("Max value: 0x{:08X}".format(max_value))
        
        # Show analysis results
        result_text = "\n".join(analysis_results)
        
        analysis_window = tk.Toplevel(self.root)
        analysis_window.title("Format Analysis Results")
        analysis_window.geometry("600x400")
        
        text_widget = tk.Text(analysis_window, wrap=tk.WORD)
        scrollbar = tk.Scrollbar(analysis_window, orient=tk.VERTICAL, command=text_widget.yview)
        text_widget.configure(yscrollcommand=scrollbar.set)
        
        text_widget.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        text_widget.insert(tk.END, result_text)
        text_widget.config(state=tk.DISABLED)
        
        # Update analysis label with first likely result
        if analysis_results:
            first_result = analysis_results[0] if analysis_results[0].startswith("LIKELY") else "Multiple possibilities found"
            self.analysis_label.config(text=first_result, fg="green")

    def quick_test_95_to_0(self):
        """Quick test: subtract 95 to map 95-125 to 0-30"""
        self.brightness_subtract_var.set("95")
        self.brightness_shift_var.set("0")
        self.brightness_scale_var.set("1.0")
        self.render_sprites()
        
    def quick_test_divide_4(self):
        """Quick test: divide by 4 to map 95-125 to ~24-31"""
        self.brightness_subtract_var.set("0")
        self.brightness_shift_var.set("2")  # Right shift by 2 = divide by 4
        self.brightness_scale_var.set("1.0")
        self.render_sprites()

    def yuv_to_rgb_conversion(self, Y, U, V):
        """Convert YUV to RGB using BT.601 full range"""
        _U = U - 128
        _V = V - 128

        R = Y + 1.402 * _V
        G = Y - 0.344136 * _U - 0.714136 * _V
        B = Y + 1.772 * _U

        R = max(0, min(255, int(R)))
        G = max(0, min(255, int(G)))
        B = max(0, min(255, int(B)))

        return (R, G, B)

    def apply_brightness_mapping(self, pixel_value):
        """Apply brightness mapping transformations"""
        try:
            # Apply right shift (divide by power of 2)
            shift = int(self.brightness_shift_var.get())
            if shift > 0:
                pixel_value = pixel_value >> shift
            
            # Subtract offset
            subtract = int(self.brightness_subtract_var.get())
            pixel_value = max(0, pixel_value - subtract)
            
            # Apply scale
            scale = float(self.brightness_scale_var.get())
            if scale != 1.0:
                pixel_value = int(pixel_value * scale)
            
            # Clamp to valid range
            pixel_value = max(0, min(255, pixel_value))
            
        except:
            pass  # If parsing fails, return original value
            
        return pixel_value
        """Convert YUV to RGB using BT.601 full range"""
        _U = U - 128
        _V = V - 128

        R = Y + 1.402 * _V
        G = Y - 0.344136 * _U - 0.714136 * _V
        B = Y + 1.772 * _U

        R = max(0, min(255, int(R)))
        G = max(0, min(255, int(G)))
        B = max(0, min(255, int(B)))

        return (R, G, B)

    def load_palette_file(self):
        """Load a separate palette file"""
        filename = tkFileDialog.askopenfilename(
            title="Select palette file",
            filetypes=[("Palette files", "*.txt *.bin"), ("All files", "*.*")]
        )

        if filename:
            try:
                palette_data = self.parse_image_data_from_file(filename)
                self.load_palette_from_data(palette_data, os.path.basename(filename))
                self.palette_label.config(text="Palette: {}".format(os.path.basename(filename)), fg="green")
                self.status_var.set("Loaded custom palette (YUV interpreted) from {}".format(os.path.basename(filename)))
                if self.current_sprites:
                    self.render_sprites()
            except Exception as e:
                tkMessageBox.showerror("Error", "Failed to load palette: {}".format(str(e)))

    def load_palette_from_data(self, palette_data, filename=""):
        """Load palette from parsed 32-bit integer data, interpreting as YUV"""
        palette = []
        
        # Handle palette offset and count
        try:
            offset = int(self.palette_offset_var.get())
            count = int(self.palette_count_var.get())
        except:
            offset = 0
            count = 256
        
        if len(palette_data) > 0 and palette_data[0] < 1000:
            color_count = palette_data[0]
            color_data = palette_data[1+offset:1+offset+min(count, color_count)]
            palette_type = "counted"
        else:
            color_data = palette_data[offset:offset+count]
            palette_type = "direct"
        
        for entry in color_data:
            processed_entry = entry
            if self.little_endian_var.get(): 
                processed_entry = self.swap_endian_32(entry) 
            
            Y = (processed_entry >> 16) & 0xFF
            U = (processed_entry >> 8) & 0xFF
            V = processed_entry & 0xFF
            
            r, g, b = self.yuv_to_rgb_conversion(Y, U, V) 
            palette.extend([r, g, b])

        if len(palette) > 768:
            palette = palette[:768]
        elif len(palette) < 768:
            palette.extend([0] * (768 - len(palette)))
            
        self.current_palette = palette
        
        actual_colors = len(color_data)
        if palette_type == "counted":
            status_msg = "Loaded palette '{}' (YUV interpreted, counted format: {} colors, offset: {})".format(filename, actual_colors, offset)
        else:
            status_msg = "Loaded palette '{}' (YUV interpreted, direct format: {} entries, offset: {})".format(filename, actual_colors, offset)
        
        self.status_var.set(status_msg)

    def browse_palettes(self):
        """Browse and show available palette files in current directory"""
        if not hasattr(self, 'current_file_path'):
            tkMessageBox.showwarning("Warning", "Load a sprite file first to determine directory")
            return
            
        current_dir = os.path.dirname(self.current_file_path)
        
        palette_files = []
        if os.path.exists(current_dir):
            for filename in os.listdir(current_dir):
                if filename.lower().endswith('.txt') and 'pal' in filename.lower():
                    palette_files.append(filename)
        
        if not palette_files:
            tkMessageBox.showinfo("No Palettes", "No palette files (containing 'pal') found in current directory")
            return
            
        palette_window = tk.Toplevel(self.root)
        palette_window.title("Select Palette")
        palette_window.geometry("400x300")
        
        tk.Label(palette_window, text="Available Palettes:").pack(pady=5)
        
        listbox = tk.Listbox(palette_window)
        listbox.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)
        
        for pfile in sorted(palette_files):
            listbox.insert(tk.END, pfile)
            
        def load_selected():
            selection = listbox.curselection()
            if selection:
                selected_file = palette_files[selection[0]]
                full_path = os.path.join(current_dir, selected_file)
                try:
                    palette_data = self.parse_image_data_from_file(full_path)
                    self.load_palette_from_data(palette_data, selected_file)
                    self.palette_label.config(text="Palette: {}".format(selected_file), fg="green")
                    if self.current_sprites:
                        self.render_sprites()
                    palette_window.destroy()
                except Exception as e:
                    tkMessageBox.showerror("Error", "Failed to load palette: {}".format(str(e)))
        
        tk.Button(palette_window, text="Load Selected", command=load_selected).pack(pady=5)
        tk.Button(palette_window, text="Cancel", command=palette_window.destroy).pack()

    def try_auto_load_palette(self, sprite_filename):
        """Try to automatically load a palette file from the same directory"""
        if self.current_palette:
            return  
        
        self.current_file_path = sprite_filename  
        sprite_dir = os.path.dirname(sprite_filename)
        sprite_base = os.path.splitext(os.path.basename(sprite_filename))[0].lower()
        
        # Enhanced palette file patterns to look for
        palette_patterns = [
            "pal{}.txt".format(sprite_base),
            "{}pal.txt".format(sprite_base), 
            "{}_pal.txt".format(sprite_base),
            "pal_{}.txt".format(sprite_base),
            "palcar.txt", "palmenu.txt", "palradiobg.txt", 
            "palbg.txt", "palframe.txt", "palicon.txt", "palette.txt",
            "palPowerOnMenu.txt", "palMenu.txt"
        ]
        
        # Also try finding any file with "pal" in the name
        try:
            all_files = os.listdir(sprite_dir)
            for filename in all_files:
                if filename.lower().endswith('.txt') and 'pal' in filename.lower():
                    if filename.lower() not in [p.lower() for p in palette_patterns]:
                        palette_patterns.append(filename)
        except:
            pass
        
        for pattern in palette_patterns:
            palette_path = os.path.join(sprite_dir, pattern)
            if os.path.exists(palette_path):
                try:
                    palette_data = self.parse_image_data_from_file(palette_path)
                    self.load_palette_from_data(palette_data, pattern)
                    self.palette_label.config(text="Auto (YUV): {}".format(pattern), fg="blue")
                    self.status_var.set("Auto-loaded palette (YUV interpreted): {}".format(pattern))
                    return
                except:
                    continue
        
        self.palette_label.config(text="No palette found - try Browse Palettes", fg="orange")

    def parse_sprites(self, data):
        """Parse sprite data - enhanced with more format detection"""
        sprites = []
        
        # Method 1: Try as header + data format (magic, version, width, height)
        if len(data) >= 4:
            magic = data[0]
            version_or_count = data[1] 
            width = data[2]
            height = data[3]
            
            if (width > 0 and height > 0 and width <= 512 and height <= 512):
                pixel_data = data[4:]
                total_pixels = width * height
                
                # Check various pixel packing schemes
                expected_len_4pix = total_pixels // 4
                expected_len_8pix = total_pixels // 8
                
                if len(pixel_data) >= expected_len_4pix:
                    sprites.append({
                        'magic': magic,
                        'version_or_count': version_or_count,
                        'width': width,
                        'height': height,
                        'pixels': pixel_data[:expected_len_4pix],
                        'format': 'header_4pix_per_word'
                    })
                
                if len(pixel_data) >= expected_len_8pix:
                    sprites.append({
                        'magic': magic,
                        'version_or_count': version_or_count,
                        'width': width,
                        'height': height,
                        'pixels': pixel_data[:expected_len_8pix],
                        'format': 'header_8pix_per_word'
                    })
        
        # Method 2: Try as raw data with common dimensions
        common_sizes = [
            (8, 8), (16, 16), (24, 24), (32, 32), (48, 48), (64, 64),
            (16, 8), (32, 16), (64, 32), (48, 40), (80, 60), (96, 64)
        ]
        
        for width, height in common_sizes:
            total_pixels = width * height
            
            expected_len_4pix = total_pixels // 4
            expected_len_8pix = total_pixels // 8
            
            if len(data) == expected_len_4pix:
                sprites.append({
                    'magic': 0,
                    'version_or_count': 0,
                    'width': width,
                    'height': height,
                    'pixels': data,
                    'format': 'raw_{}x{}_4pix'.format(width, height)
                })
            
            if len(data) == expected_len_8pix:
                sprites.append({
                    'magic': 0,
                    'version_or_count': 0,
                    'width': width,
                    'height': height,
                    'pixels': data,
                    'format': 'raw_{}x{}_8pix'.format(width, height)
                })
        
        if not sprites:
            # Fallback: assume it's raw 32x32 4-pixel packed
            sprites.append({
                'magic': 0,
                'version_or_count': 0,
                'width': 32,
                'height': 32,
                'pixels': data[:256] if len(data) >= 256 else data,
                'format': 'fallback_32x32_4pix'
            })
            
        return sprites
    
    def swap_endian_32(self, value):
        """Swap bytes in a 32-bit value"""
        return ((value & 0xFF) << 24) | (((value >> 8) & 0xFF) << 16) | \
               (((value >> 16) & 0xFF) << 8) | ((value >> 24) & 0xFF)
    
    def update_sprite_menu(self):
        """Update the sprite interpretation menu"""
        if not self.current_sprites:
            menu = self.sprite_menu['menu']
            menu.delete(0, 'end')
            menu.add_command(label="No interpretations found", command=tk._setit(self.sprite_var, "0"))
            self.sprite_var.set("0")
            self.info_label.config(text="Sprite Info: -")
            return
            
        menu = self.sprite_menu['menu']
        menu.delete(0, 'end')
        
        for i, sprite in enumerate(self.current_sprites):
            label = "{}: {}x{} ({})".format(i, sprite['width'], sprite['height'], 
                                             sprite.get('format', 'unknown'))
            menu.add_command(label=label, command=tk._setit(self.sprite_var, str(i)))
        
        current_selection = self.sprite_var.get()
        if not current_selection.isdigit() or int(current_selection) >= len(self.current_sprites):
            self.sprite_var.set("0")
        
        selected_sprite_index = int(self.sprite_var.get())
        if selected_sprite_index < len(self.current_sprites):
            sprite = self.current_sprites[selected_sprite_index]
            info_text = "Interpretation: {}x{} ({})".format(
                sprite['width'], sprite['height'], sprite.get('format', 'unknown'))
            self.info_label.config(text=info_text)
    
    def render_sprites(self):
        """Render the selected sprite with the chosen format"""
        if not self.current_sprites:
            self.display_image(Image.new('RGB', (1,1), 'black'), 
                             {'width': 1, 'height': 1, 'format': 'No data'})
            return
            
        try:
            sprite_index = int(self.sprite_var.get())
            if sprite_index >= len(self.current_sprites):
                sprite_index = 0
                self.sprite_var.set("0") 

            sprite = self.current_sprites[sprite_index]
            format_type = self.format_var.get()
            scale = int(self.scale_var.get())
            
            image = None
            if format_type == "8bpp_indexed":
                image = self.render_8bpp_indexed(sprite)
            elif format_type == "4bpp_indexed":
                image = self.render_4bpp_indexed(sprite)
            elif format_type == "2bpp_indexed":
                image = self.render_2bpp_indexed(sprite)
            elif format_type == "1bpp_mono":
                image = self.render_1bpp_mono(sprite)
            else:
                raise ValueError("Unknown format: {}".format(format_type))
            
            if scale > 1:
                new_size = (image.width * scale, image.height * scale)
                image = image.resize(new_size, Image.NEAREST)
            
            self.display_image(image, sprite)
            
            status_parts = ["Rendered sprite {} as {} ({}x{} scaled {}x)".format(
                sprite_index, format_type, sprite['width'], sprite['height'], scale)]
            
            if self.invert_colors_var.get():
                status_parts.append("colors inverted")
            if self.invert_palette_var.get():
                status_parts.append("palette indices inverted")
            if self.little_endian_var.get():
                status_parts.append("little endian")
            
            status_msg = status_parts[0]
            if len(status_parts) > 1:
                status_msg += " - " + ", ".join(status_parts[1:])
            
            self.status_var.set(status_msg)
            
        except Exception as e:
            tkMessageBox.showerror("Render Error", "Error rendering sprite: {}".format(str(e)))
            self.display_image(Image.new('RGB', (1,1), 'red'), 
                             {'width': 1, 'height': 1, 'format': 'Error'})
    
    def render_8bpp_indexed(self, sprite):
        """Render as 8-bit indexed color (4 pixels per 32-bit word)"""
        if not self.current_palette:
            palette = []
            for i in range(256):
                gray = i
                palette.extend([gray, gray, gray])
            tkMessageBox.showwarning("Warning", "No palette loaded. Using default grayscale.")
        else:
            palette = self.current_palette[:]

        if self.invert_colors_var.get():
            for i in range(0, len(palette), 3):
                palette[i] = 255 - palette[i]
                palette[i+1] = 255 - palette[i+1]
                palette[i+2] = 255 - palette[i+2]

        width = sprite['width']
        height = sprite['height']
        image = Image.new('P', (width, height))
        image.putpalette(palette)
        pixels = []
        
        # Packed mode: extract 4 pixels from each 32-bit word
        for pixel_val in sprite['pixels']:
            if self.little_endian_var.get():
                pixel_val = self.swap_endian_32(pixel_val)
            
            # Extract 4 bytes (pixels) from the 32-bit value
            p1 = (pixel_val >> 24) & 0xFF
            p2 = (pixel_val >> 16) & 0xFF
            p3 = (pixel_val >> 8) & 0xFF
            p4 = pixel_val & 0xFF
            
            # Apply brightness mapping to each pixel
            p1 = self.apply_brightness_mapping(p1)
            p2 = self.apply_brightness_mapping(p2)
            p3 = self.apply_brightness_mapping(p3)
            p4 = self.apply_brightness_mapping(p4)
            
            if self.invert_palette_var.get():
                p1 = 255 - p1
                p2 = 255 - p2
                p3 = 255 - p3
                p4 = 255 - p4
            
            pixels.extend([p1, p2, p3, p4])
        
        # Trim or pad pixels to match image size
        expected_pixels = width * height
        if len(pixels) > expected_pixels:
            pixels = pixels[:expected_pixels]
        elif len(pixels) < expected_pixels:
            pixels.extend([0] * (expected_pixels - len(pixels)))
        
        image.putdata(pixels)
        return image.convert('RGB')
    
    def render_4bpp_indexed(self, sprite):
        """Render as 4-bit indexed (8 pixels per 32-bit value)"""
        if not self.current_palette:
            palette = []
            for i in range(16):
                gray = i * 17  
                palette.extend([gray, gray, gray])
            for i in range(16, 256):
                palette.extend([0, 0, 0])
            tkMessageBox.showwarning("Warning", "No palette loaded. Using default 4-bit grayscale.")
        else:
            palette = self.current_palette[:]

        if self.invert_colors_var.get():
            for i in range(0, len(palette), 3):
                palette[i] = 255 - palette[i]
                palette[i+1] = 255 - palette[i+1]
                palette[i+2] = 255 - palette[i+2]

        width = sprite['width']
        height = sprite['height']
        image = Image.new('P', (width, height))
        image.putpalette(palette)
        pixels = []
        
        for pixel_val in sprite['pixels']:
            if self.little_endian_var.get():
                pixel_val = self.swap_endian_32(pixel_val)
            
            # Extract 8 nibbles from the 32-bit value
            for shift in [28, 24, 20, 16, 12, 8, 4, 0]:
                pixel_index = (pixel_val >> shift) & 0xF
                
                if self.invert_palette_var.get():
                    pixel_index = 15 - pixel_index
                
                pixels.append(pixel_index)
        
        expected_pixels = width * height
        if len(pixels) > expected_pixels:
            pixels = pixels[:expected_pixels]
        elif len(pixels) < expected_pixels:
            pixels.extend([0] * (expected_pixels - len(pixels)))

        image.putdata(pixels)
        return image.convert('RGB')
    
    def render_2bpp_indexed(self, sprite):
        """Render as 2-bit indexed (16 pixels per 32-bit value)"""
        if not self.current_palette:
            palette = []
            for i in range(4):
                gray = i * 85 
                palette.extend([gray, gray, gray])
            for i in range(4, 256):
                palette.extend([0, 0, 0])
            tkMessageBox.showwarning("Warning", "No palette loaded. Using default 2-bit grayscale.")
        else:
            palette = self.current_palette[:]

        if self.invert_colors_var.get():
            for i in range(0, len(palette), 3):
                palette[i] = 255 - palette[i]
                palette[i+1] = 255 - palette[i+1]
                palette[i+2] = 255 - palette[i+2]

        width = sprite['width']
        height = sprite['height']
        image = Image.new('P', (width, height))
        image.putpalette(palette)
        pixels = []
        
        for pixel_val in sprite['pixels']:
            if self.little_endian_var.get():
                pixel_val = self.swap_endian_32(pixel_val)
            
            # Extract 16 2-bit values from the 32-bit word
            for shift in range(30, -1, -2):
                pixel_index = (pixel_val >> shift) & 0x3
                
                if self.invert_palette_var.get():
                    pixel_index = 3 - pixel_index
                
                pixels.append(pixel_index)
        
        expected_pixels = width * height
        if len(pixels) > expected_pixels:
            pixels = pixels[:expected_pixels]
        elif len(pixels) < expected_pixels:
            pixels.extend([0] * (expected_pixels - len(pixels)))

        image.putdata(pixels)
        return image.convert('RGB')
    
    def render_1bpp_mono(self, sprite):
        """Render as 1-bit monochrome (32 pixels per 32-bit value)"""
        width = sprite['width']
        height = sprite['height']
        image = Image.new('1', (width, height))
        pixels = []
        
        for pixel_val in sprite['pixels']:
            if self.little_endian_var.get():
                pixel_val = self.swap_endian_32(pixel_val)
            
            # Extract 32 bits from the 32-bit word
            for shift in range(31, -1, -1):
                bit_val = (pixel_val >> shift) & 0x1
                
                if self.invert_colors_var.get():
                    bit_val = 1 - bit_val
                
                pixels.append(bit_val)
        
        expected_pixels = width * height
        if len(pixels) > expected_pixels:
            pixels = pixels[:expected_pixels]
        elif len(pixels) < expected_pixels:
            pixels.extend([0] * (expected_pixels - len(pixels)))

        image.putdata(pixels)
        return image.convert('RGB')

    def show_palette_debug(self):
        """Show detailed palette debugging information"""
        if not self.current_palette:
            tkMessageBox.showwarning("Warning", "No palette loaded")
            return
            
        debug_window = tk.Toplevel(self.root)
        debug_window.title("Palette Debug Information")
        debug_window.geometry("800x600")
        
        text_widget = tk.Text(debug_window, wrap=tk.WORD, font=("Courier", 10))
        scrollbar = tk.Scrollbar(debug_window, orient=tk.VERTICAL, command=text_widget.yview)
        text_widget.configure(yscrollcommand=scrollbar.set)
        
        text_widget.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        # Analyze palette
        info = []
        info.append("PALETTE ANALYSIS")
        info.append("================")
        info.append("Total palette entries: {}".format(len(self.current_palette) // 3))
        info.append("")
        
        # Show first 32 colors in detail
        info.append("First 32 Colors (RGB values):")
        info.append("-" * 50)
        for i in range(0, min(96, len(self.current_palette)), 3):
            color_idx = i // 3
            r, g, b = self.current_palette[i], self.current_palette[i+1], self.current_palette[i+2]
            hex_color = "#{:02x}{:02x}{:02x}".format(r, g, b)
            info.append("Color {:2d}: RGB({:3d},{:3d},{:3d}) {}".format(color_idx, r, g, b, hex_color))
        
        info.append("")
        info.append("Color Distribution Analysis:")
        info.append("-" * 30)
        
        # Analyze color distribution
        unique_colors = set()
        dark_colors = 0
        bright_colors = 0
        
        for i in range(0, len(self.current_palette), 3):
            r, g, b = self.current_palette[i], self.current_palette[i+1], self.current_palette[i+2]
            unique_colors.add((r, g, b))
            brightness = (r + g + b) // 3
            if brightness < 64:
                dark_colors += 1
            elif brightness > 192:
                bright_colors += 1
        
        info.append("Unique colors: {}".format(len(unique_colors)))
        info.append("Dark colors (< 64): {}".format(dark_colors))
        info.append("Bright colors (> 192): {}".format(bright_colors))
        
        text_widget.insert(tk.END, "\n".join(info))
        text_widget.config(state=tk.DISABLED)

    def show_sprite_debug(self):
        """Show detailed sprite data debugging information"""
        if not self.current_sprites:
            tkMessageBox.showwarning("Warning", "No sprite data loaded")
            return
            
        debug_window = tk.Toplevel(self.root)
        debug_window.title("Sprite Debug Information")  
        debug_window.geometry("800x600")
        
        text_widget = tk.Text(debug_window, wrap=tk.WORD, font=("Courier", 10))
        scrollbar = tk.Scrollbar(debug_window, orient=tk.VERTICAL, command=text_widget.yview)
        text_widget.configure(yscrollcommand=scrollbar.set)
        
        text_widget.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        sprite_index = int(self.sprite_var.get()) if self.sprite_var.get().isdigit() else 0
        if sprite_index >= len(self.current_sprites):
            sprite_index = 0
            
        sprite = self.current_sprites[sprite_index]
        
        info = []
        info.append("SPRITE DATA ANALYSIS")
        info.append("====================")
        info.append("Sprite: {}".format(sprite_index))
        info.append("Dimensions: {}x{}".format(sprite['width'], sprite['height']))
        info.append("Format: {}".format(sprite['format']))
        info.append("Magic: 0x{:08X}".format(sprite.get('magic', 0)))
        info.append("Data length: {} words".format(len(sprite['pixels'])))
        info.append("")
        
        # Analyze pixel data
        pixel_data = sprite['pixels'][:16]  # First 16 words
        info.append("First 16 Data Words:")
        info.append("-" * 40)
        for i, word in enumerate(pixel_data):
            # Show word in different interpretations
            info.append("Word {:2d}: 0x{:08X}".format(i, word))
            
            # Show as 4 bytes (8-bit pixels)
            bytes_8bit = [
                (word >> 24) & 0xFF,
                (word >> 16) & 0xFF, 
                (word >> 8) & 0xFF,
                word & 0xFF
            ]
            info.append("  8-bit: [{:3d},{:3d},{:3d},{:3d}]".format(*bytes_8bit))
            
            # Show as 8 nibbles (4-bit pixels)
            nibbles_4bit = []
            for shift in [28, 24, 20, 16, 12, 8, 4, 0]:
                nibbles_4bit.append((word >> shift) & 0xF)
            info.append("  4-bit: [{}]".format(",".join("{:2d}".format(n) for n in nibbles_4bit)))
            info.append("")
        
        # Value distribution analysis
        info.append("Value Distribution Analysis:")
        info.append("-" * 35)
        
        all_8bit_values = []
        all_4bit_values = []
        
        for word in sprite['pixels']:
            # Collect 8-bit values
            for shift in [24, 16, 8, 0]:
                all_8bit_values.append((word >> shift) & 0xFF)
            
            # Collect 4-bit values
            for shift in [28, 24, 20, 16, 12, 8, 4, 0]:
                all_4bit_values.append((word >> shift) & 0xF)
        
        info.append("8-bit pixel analysis:")
        info.append("  Unique values: {}".format(len(set(all_8bit_values))))
        info.append("  Min value: {}".format(min(all_8bit_values)))
        info.append("  Max value: {}".format(max(all_8bit_values)))
        
        info.append("")
        info.append("4-bit pixel analysis:")
        info.append("  Unique values: {}".format(len(set(all_4bit_values))))
        info.append("  Min value: {}".format(min(all_4bit_values)))
        info.append("  Max value: {}".format(max(all_4bit_values)))
        
        # Recommendations
        info.append("")
        info.append("RECOMMENDATIONS:")
        info.append("================")
        
        unique_8bit = len(set(all_8bit_values))
        unique_4bit = len(set(all_4bit_values))
        max_8bit = max(all_8bit_values)
        max_4bit = max(all_4bit_values)
        
        if unique_4bit <= 16 and max_4bit <= 15:
            info.append("✓ Try 4bpp_indexed format - pixel values fit in 4-bit range")
        if unique_8bit <= 256 and max_8bit <= 255:
            info.append("✓ Try 8bpp_indexed format - pixel values fit in 8-bit range")
        if max_8bit > 200:
            info.append("⚠ High pixel values detected - might need palette offset adjustment")
        if unique_4bit <= 4:
            info.append("✓ Try 2bpp_indexed format - only {} unique 4-bit values".format(unique_4bit))
        
        text_widget.insert(tk.END, "\n".join(info))
        text_widget.config(state=tk.DISABLED)

    def display_image(self, image, sprite_info):
        """Display the rendered image"""
        for widget in self.image_frame.winfo_children():
            widget.destroy()
        
        photo = ImageTk.PhotoImage(image)
        self.image_label = tk.Label(self.image_frame, image=photo)
        self.image_label.image = photo 
        self.image_label.pack(expand=True, pady=10)
        
        sprite_format = sprite_info.get('format', 'unknown')
        info_text = "{}x{} sprite (Format: {})".format(
            sprite_info['width'], sprite_info['height'], sprite_format)
        if sprite_info.get('magic', 0) != 0:
            info_text += " (Magic: 0x{:08X})".format(sprite_info['magic'])
        tk.Label(self.image_frame, text=info_text).pack()
        
        current_format = getattr(self, 'format_var', None)
        if current_format and 'indexed' in current_format.get():
            if self.current_palette:
                palette_name = self.palette_label.cget("text").replace("Palette: ", "").replace("Auto (YUV): ", "")
                palette_colors = len(self.current_palette) // 3
                palette_info = "Using YUV-interpreted palette: {} ({} colors)".format(palette_name, palette_colors)
                color = "blue"
            else:
                palette_info = "WARNING: INDEXED FORMAT NEEDS PALETTE - use 'Browse Palettes' or 'Load Palette File'"
                color = "red"
                
            tk.Label(self.image_frame, text=palette_info, fg=color, wraplength=400).pack()
        
        # Processing options status
        options = []
        if self.invert_colors_var.get():
            options.append("Colors inverted")
        if self.invert_palette_var.get():
            options.append("Palette indices inverted")
        if self.little_endian_var.get():
            options.append("Little endian")
        
        # Show brightness mapping if applied
        try:
            shift = int(self.brightness_shift_var.get())
            subtract = int(self.brightness_subtract_var.get())
            scale = float(self.brightness_scale_var.get())
            
            if shift > 0 or subtract > 0 or scale != 1.0:
                mapping_parts = []
                if shift > 0:
                    mapping_parts.append(">>{}".format(shift))
                if subtract > 0:
                    mapping_parts.append("-{}".format(subtract))
                if scale != 1.0:
                    mapping_parts.append("*{:.1f}".format(scale))
                options.append("Brightness: " + " ".join(mapping_parts))
        except:
            pass
            
        if options:
            tk.Label(self.image_frame, text=" | ".join(options), fg="blue").pack()
        
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
                tkMessageBox.showinfo("Success", "Image saved successfully to:\n{}".format(filename))
            except Exception as e:
                tkMessageBox.showerror("Error", "Failed to save image: {}".format(str(e)))
    
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
    if len(sys.argv) > 1 and sys.argv[1] in ['-h', '--help']:
        print("Enhanced CT952 Digital Picture Frame Sprite/Icon Browser")
        print("Usage: python {} [sprite_file.txt]".format(sys.argv[0]))
        print("\nThis enhanced tool provides better format detection and debugging")
        print("for sprite/icon data from the ct952-dmp-121 project.")
        print("\nKey features:")
        print("- Auto-format analysis to suggest the best interpretation")
        print("- Palette debugging tools with offset/count controls")
        print("- Sprite data analysis and recommendations")
        print("- YUV palette interpretation")
        print("\nWorkflow for car_red + palcar issue:")
        print("1. Load car_red.txt")
        print("2. Click 'Show Sprite Data' - see what pixel values are used")
        print("3. Load palcar.txt")
        print("4. Click 'Show Palette Info' - analyze palette colors") 
        print("5. Try different format (4bpp vs 8bpp)")
        print("6. Try palette offset = 1 if still black")
        print("7. Experiment with Invert options")
        print("\nRequires: PIL/Pillow, Tkinter")
        return
    
    try:
        app = SpriteBrowser()
        app.run()
    except ImportError as e:
        print("Error: Missing required library. Please install Pillow.")
        print("You can install it with: pip install Pillow")
        sys.exit(1)
    except Exception as e:
        print("Error starting application: {}".format(str(e)))
        sys.exit(1)

if __name__ == "__main__":
    main()