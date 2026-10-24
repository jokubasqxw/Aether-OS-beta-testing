import socket, subprocess, time, os
from PIL import Image

def test_driver(vga_mode, extra_args, test_name):
    sock_path = f'/tmp/qemu_gfx_{test_name}.sock'
    ppm_path = f'/tmp/qemu_gfx_{test_name}.ppm'
    png_path = f'/tmp/qemu_gfx_{test_name}.png'

    if os.path.exists(sock_path):
        os.remove(sock_path)
    if os.path.exists(ppm_path):
        os.remove(ppm_path)

    print(f"\n=======================================================")
    print(f"Testing Graphics Driver: {vga_mode} ({test_name})")
    print(f"=======================================================")

    cmd = [
        'qemu-system-x86_64',
        '-kernel', 'build/aetheros32.bin',
        '-serial', 'stdio',
        '-m', '4096M',
        '-vga', vga_mode,
        *extra_args,
        '-netdev', 'user,id=net0',
        '-device', 'e1000,netdev=net0',
        '-audiodev', 'pa,id=snd0',
        '-device', 'AC97,audiodev=snd0',
        '-display', 'none',
        '-monitor', f'unix:{sock_path},server,nowait'
    ]

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(1.2)

    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock_path)

        def send_mon(cmd_str):
            s.sendall(cmd_str.encode() + b'\n')
            time.sleep(0.05)

        def type_str(text):
            for ch in text:
                if ch == ' ':
                    send_mon('sendkey spc')
                elif ch == '\n':
                    send_mon('sendkey ret')
                elif ch == '-':
                    send_mon('sendkey minus')
                elif ch == '_':
                    send_mon('sendkey shift-minus')
                elif ch == '.':
                    send_mon('sendkey dot')
                else:
                    send_mon(f'sendkey {ch}')
                time.sleep(0.03)

        # 1. Run 'gpu' command to verify hardware driver detection
        print("Executing 'gpu' command...")
        type_str("gpu\n")
        time.sleep(0.4)

        # 2. Run 'pat' command
        print("Executing 'pat' command...")
        type_str("pat\n")
        time.sleep(0.4)

        # 3. Launch Desktop GUI
        print("Launching Desktop GUI 1080p...")
        type_str("gui 1080p\n")
        time.sleep(1.5)

        # Take screenshot of desktop GUI
        send_mon(f'screendump {ppm_path}')
        time.sleep(0.5)

        # Exit GUI via Esc or menu
        send_mon('sendkey esc')
        time.sleep(0.3)

        send_mon('quit')
        s.close()
    except Exception as e:
        print(f"Exception during socket comm: {e}")

    try:
        stdout, stderr = proc.communicate(timeout=5)
    except Exception:
        proc.kill()
        stdout, stderr = proc.communicate()

    print(f"\n--- Serial Output ({test_name}) ---")
    for line in stdout.splitlines():
        if any(k in line for k in ['[OK]', 'Graphics', 'Driver', 'BGA', 'SVGA', 'PAT', 'Hardware', 'Active']):
            print(line)

    if os.path.exists(ppm_path):
        im = Image.open(ppm_path)
        im.save(png_path)
        print(f"[SUCCESS] Saved screenshot: {png_path} ({im.size[0]}x{im.size[1]})")
        return True
    else:
        print(f"[WARN] No screendump produced.")
        return False

# Test 1: Bochs VBE (Standard VGA) with software backbuffer and FIFO transfer
res1 = test_driver('std', ['-global', 'VGA.vgamem_mb=32'], 'bga_hw_double_buffer')

# Test 2: VMware SVGA II with Hardware 2D FIFO Acceleration
res2 = test_driver('vmware', [], 'vmware_svga_fifo')

print("\n=======================================================")
print(f"Test Results: Bochs VBE = {res1}, VMware SVGA II = {res2}")
print("=======================================================")
