import socket, subprocess, time, os
from PIL import Image

sock_path = '/tmp/qemu_net_sound.sock'
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

proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
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
        else:
            send_mon(f'sendkey {ch}')
        time.sleep(0.04)

# 1. Test CLI Shell commands first
print("Running CLI commands...")
type_str("ifconfig\n")
time.sleep(0.4)

type_str("ping 10.0.2.2 1\n")
time.sleep(0.8)

type_str("sound\n")
time.sleep(0.4)

type_str("play chime\n")
time.sleep(0.6)

type_str("wavplay beep.wav\n")
time.sleep(0.6)

# 2. Launch GUI Desktop 1080p
print("Launching GUI desktop...")
type_str("desktop 1080p\n")
time.sleep(1.5)

# Focus Terminal window (click Terminal taskbar button at x=290, y=18)
send_mon('mouse_move -670 -522') # move from (960, 540) to (290, 18)
time.sleep(0.1)
send_mon('mouse_button 1')
time.sleep(0.1)
send_mon('mouse_button 0')
time.sleep(0.4)

# In GUI terminal, run ifconfig and sound
print("Running 'ifconfig' and 'sound' inside GUI terminal...")
type_str("ifconfig\n")
time.sleep(0.6)

type_str("sound\n")
time.sleep(0.6)

type_str("play coin\n")
time.sleep(0.6)

send_mon('screendump /tmp/gui_net_sound.ppm')
time.sleep(0.4)

send_mon('quit')
try:
    stdout, stderr = proc.communicate(timeout=5)
except Exception:
    proc.kill()
    stdout, stderr = proc.communicate()

print("Console Output Summary:")
for line in stdout.splitlines()[:40]:
    print(line)

if os.path.exists('/tmp/gui_net_sound.ppm'):
    im = Image.open('/tmp/gui_net_sound.ppm')
    im.save('/tmp/gui_net_sound.png')
    print("Screenshot saved to /tmp/gui_net_sound.png")
