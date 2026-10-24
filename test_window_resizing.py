import socket, subprocess, time, os
from PIL import Image

def run_test():
    sock_path = '/tmp/qemu_resize_test.sock'
    if os.path.exists(sock_path): os.remove(sock_path)
    for p in ['/tmp/desktop_resize_terminal.png', '/tmp/desktop_resize_sysmon.png', '/tmp/desktop_resize_3d.png']:
        if os.path.exists(p): os.remove(p)

    cmd = [
        'qemu-system-x86_64',
        '-kernel', 'build/aetheros32.bin',
        '-serial', 'stdio',
        '-m', '4096M',
        '-vga', 'std',
        '-global', 'VGA.vgamem_mb=32',
        '-netdev', 'user,id=net0',
        '-device', 'e1000,netdev=net0',
        '-audiodev', 'pa,id=snd0',
        '-device', 'AC97,audiodev=snd0',
        '-display', 'none',
        '-monitor', f'unix:{sock_path},server,nowait'
    ]

    print("[TEST] Launching QEMU...")
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    for _ in range(30):
        if os.path.exists(sock_path): break
        time.sleep(0.1)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)

    def send_mon(cmd_str):
        s.sendall(cmd_str.encode() + b'\n')
        time.sleep(0.04)

    def type_str(text):
        for ch in text:
            if ch == ' ': send_mon('sendkey spc')
            elif ch == '\n': send_mon('sendkey ret')
            elif ch == '-': send_mon('sendkey minus')
            elif ch == '_': send_mon('sendkey shift-minus')
            elif ch == '.': send_mon('sendkey dot')
            else: send_mon(f'sendkey {ch}')
            time.sleep(0.03)

    cur_mouse_x = 960
    cur_mouse_y = 540

    def move_mouse_to(target_x, target_y):
        nonlocal cur_mouse_x, cur_mouse_y
        total_dx = target_x - cur_mouse_x
        total_dy = target_y - cur_mouse_y

        # Break moves into steps of at most 40 px for smooth PS/2 packet transmission
        steps = max(1, max(abs(total_dx), abs(total_dy)) // 40 + 1)
        step_dx = total_dx / steps
        step_dy = total_dy / steps

        accum_x = 0.0
        accum_y = 0.0
        sent_x = 0
        sent_y = 0

        for i in range(steps):
            accum_x += step_dx
            accum_y += step_dy
            dx_int = int(round(accum_x)) - sent_x
            dy_int = int(round(accum_y)) - sent_y
            sent_x += dx_int
            sent_y += dy_int
            # QEMU monitor mouse_move: dx, dy are screen coordinates (positive = right/down)
            send_mon(f'mouse_move {dx_int} {dy_int}')
            time.sleep(0.02)

        cur_mouse_x = target_x
        cur_mouse_y = target_y
        time.sleep(0.05)

    def mouse_down():
        send_mon('mouse_button 1')
        time.sleep(0.08)

    def mouse_up():
        send_mon('mouse_button 0')
        time.sleep(0.08)

    def capture_screen(dest_png):
        ppm_temp = '/tmp/screen_temp.ppm'
        if os.path.exists(ppm_temp): os.remove(ppm_temp)
        send_mon(f'screendump {ppm_temp}')
        time.sleep(0.5)
        if os.path.exists(ppm_temp):
            im = Image.open(ppm_temp)
            im.save(dest_png)
            os.remove(ppm_temp)
            print(f"[OK] Captured screenshot: {dest_png} ({im.size[0]}x{im.size[1]})")
            return True
        else:
            print(f"[ERROR] Failed to capture {dest_png}")
            return False

    # 1. Wait for boot and launch GUI
    print("[TEST] Waiting for boot...")
    time.sleep(1.5)
    print("[TEST] Launching Desktop GUI at 1080p...")
    type_str('gui 1080p\n')
    time.sleep(2.0)

    # Type some text into Terminal (Window 2) so we can see lines reflow
    print("[TEST] Typing commands into Terminal...")
    type_str('help\n')
    time.sleep(0.5)
    type_str('fetch\n')
    time.sleep(0.5)

    # =========================================================================
    # Test 1: Resize Window 2 (Terminal)
    # Win2 initial: x=40, y=490, w=580, h=570. Bottom-right: (620, 1060).
    # Drag bottom-right corner to (860, 960) to expand width and adjust height.
    # =========================================================================
    print("[TEST] Resizing Window 2 (Interactive Terminal)...")
    move_mouse_to(616, 1056) # Bottom-right grip of Window 2
    time.sleep(0.1)
    mouse_down()
    move_mouse_to(860, 960)  # Expand width to 820, height to 470
    time.sleep(0.2)
    mouse_up()
    time.sleep(0.3)

    # Move cursor slightly away to show resize grip and expanded text columns
    move_mouse_to(750, 750)
    time.sleep(0.2)
    capture_screen('/tmp/desktop_resize_terminal.png')

    # =========================================================================
    # Test 2: Resize Window 1 (System Monitor)
    # Win1 initial: x=40, y=48, w=580, h=420. Bottom-right: (620, 468).
    # Drag bottom-right corner to (860, 430) to expand width and scale memory bar!
    # =========================================================================
    print("[TEST] Resizing Window 1 (System Monitor)...")
    move_mouse_to(616, 464) # Bottom-right grip of Window 1
    time.sleep(0.1)
    mouse_down()
    move_mouse_to(860, 430) # Expand width to 820
    time.sleep(0.2)
    mouse_up()
    time.sleep(0.3)

    # Move cursor slightly away
    move_mouse_to(650, 250)
    time.sleep(0.2)
    capture_screen('/tmp/desktop_resize_sysmon.png')

    # =========================================================================
    # Test 3: Launch Window 3 (3D Hardware Rendering Engine) & Resize
    # Click on [+ 3D Engine] button at (400, 15)
    # =========================================================================
    print("[TEST] Launching Window 3 (3D Engine)...")
    move_mouse_to(400, 15)
    time.sleep(0.1)
    mouse_down()
    mouse_up()
    time.sleep(1.2)

    # Win3 is at x=650, y=48, w=1230, h=1010.
    # First: Resize Window 3 from Left Edge (W): Left edge is at x=650.
    print("[TEST] Resizing Window 3 from Left Edge...")
    move_mouse_to(652, 500)
    time.sleep(0.1)
    mouse_down()
    move_mouse_to(900, 500) # Move left edge to x=900 (shrinking width)
    time.sleep(0.2)
    mouse_up()
    time.sleep(0.4)

    # Second: Resize Window 3 from bottom-right corner (SE): (1880, 1058) -> (1650, 850)
    print("[TEST] Resizing Window 3 from Bottom-Right Corner...")
    move_mouse_to(1876, 1054)
    time.sleep(0.1)
    mouse_down()
    move_mouse_to(1650, 850)
    time.sleep(0.2)
    mouse_up()
    time.sleep(0.4)

    # Move mouse inside 3D viewport
    move_mouse_to(1250, 450)
    time.sleep(0.2)
    capture_screen('/tmp/desktop_resize_3d.png')

    # Finish and cleanup
    send_mon('sendkey esc')
    time.sleep(0.3)
    send_mon('quit')
    s.close()
    proc.communicate(timeout=3)
    print("[TEST] Finished all tests successfully.")

if __name__ == '__main__':
    run_test()
