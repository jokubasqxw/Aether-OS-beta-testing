import socket, subprocess, time, os
from PIL import Image

sock_path = '/tmp/qemu_wifi_test.sock'
if os.path.exists(sock_path):
    os.remove(sock_path)

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

proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors='replace')
time.sleep(1.2)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)

def send_mon(cmd_str):
    s.sendall(cmd_str.encode() + b'\n')
    time.sleep(0.06)

def type_str(text):
    for ch in text:
        if ch == ' ':
            send_mon('sendkey spc')
        elif ch == '\n':
            send_mon('sendkey ret')
        elif ch == '"':
            send_mon('sendkey shift-apostrophe')
        elif ch == '*':
            send_mon('sendkey shift-8')
        elif ch == '+':
            send_mon('sendkey shift-equal')
        elif ch == '-':
            send_mon('sendkey minus')
        elif ch == '/':
            send_mon('sendkey slash')
        elif ch == '_':
            send_mon('sendkey shift-minus')
        elif ch == '.':
            send_mon('sendkey dot')
        elif ch.isupper():
            send_mon(f'sendkey shift-{ch.lower()}')
        else:
            send_mon(f'sendkey {ch}')
        time.sleep(0.04)

print("1. Testing CLI Wi-Fi scanning...")
type_str("wifi scan\n")
time.sleep(0.5)

print("2. Testing CLI Wi-Fi connection...")
type_str("wifi connect Aether-HyperFiber-5G pass123456\n")
time.sleep(0.5)

print("3. Testing CLI Wi-Fi status...")
type_str("wifi status\n")
time.sleep(0.5)

print("4. Testing Kernel Route table...")
type_str("route\n")
time.sleep(0.5)

print("5. Testing dual-interface ifconfig...")
type_str("ifconfig\n")
time.sleep(0.5)

print("6. Launching GUI desktop...")
type_str("desktop 1080p\n")
time.sleep(1.5)

# Focus Terminal window (click Terminal taskbar button at x=290, y=18)
send_mon('mouse_move -670 -522') # move from (960, 540) to (290, 18)
time.sleep(0.1)
send_mon('mouse_button 1')
time.sleep(0.1)
send_mon('mouse_button 0')
time.sleep(0.4)

print("7. Running 'wifi status' inside GUI terminal...")
type_str("wifi status\n")
time.sleep(0.6)

print("8. Running 'ifconfig' inside GUI terminal...")
type_str("ifconfig\n")
time.sleep(0.6)

send_mon('screendump /tmp/gui_wifi_test.ppm')
time.sleep(0.4)

send_mon('quit')
try:
    stdout, stderr = proc.communicate(timeout=3)
    print("\n--- Console Output Summary ---:\n")
    print(stdout)
except Exception as e:
    proc.kill()
    stdout, stderr = proc.communicate()
    print("\n--- Console Output Summary ---:\n")
    print(stdout)

if os.path.exists('/tmp/gui_wifi_test.ppm'):
    img = Image.open('/tmp/gui_wifi_test.ppm')
    img.save('/tmp/gui_wifi_test.png')
    print("Screenshot saved to /tmp/gui_wifi_test.png")
