import sys

def main():
    if len(sys.argv) < 3:
        print("Usage: ttf_conv.py <input.ttf> <output.h>")
        sys.exit(1)
    
    with open(sys.argv[1], 'rb') as f:
        data = f.read()
    
    with open(sys.argv[2], 'w') as f:
        f.write("#ifndef KERNEL_FONT_DATA_HPP\n")
        f.write("#define KERNEL_FONT_DATA_HPP\n\n")
        f.write("#include \"types.hpp\"\n\n")
        f.write("namespace font {\n")
        f.write(f"constexpr size_t TTF_FONT_SIZE = {len(data)};\n")
        f.write("extern const uint8_t TTF_FONT_DATA[];\n")
        f.write("} // namespace font\n\n")
        f.write("#endif // KERNEL_FONT_DATA_HPP\n")
    
    # Write the .cpp file
    cpp_path = sys.argv[2].replace('.hpp', '.cpp')
    with open(cpp_path, 'w') as f:
        f.write('#include "font_data.hpp"\n\n')
        f.write('namespace font {\n')
        f.write('const uint8_t TTF_FONT_DATA[] = {\n')
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            f.write('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',\n')
        f.write('};\n')
        f.write('} // namespace font\n')
    
    print(f"Converted {len(data)} bytes to {sys.argv[2]} and {cpp_path}")

if __name__ == '__main__':
    main()
