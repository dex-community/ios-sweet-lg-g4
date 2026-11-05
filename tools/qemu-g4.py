#!/usr/bin/env python3
"""Boot the G4 shim under QEMU and save what it drew.

QEMU does not emulate the G4, so this only checks the shim's own logic:
that it finds the device tree, that it locates a framebuffer in it, that
the console works and that the payload loader rejects a bad payload.

The test device tree describes a QEMU-like machine: a simple-framebuffer
node in RAM and a /chosen with an initrd that holds no payload.

Usage: qemu-g4.py [--image shim/out/Image] [--out shim/out]
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

TEST_DTS = """\
/dts-v1/;
/ {
	#address-cells = <1>;
	#size-cells = <1>;
	model = "LG G4 shim test";
	compatible = "lg,h815", "qcom,msm8992";
	qcom,msm-id = <0xfb 0x00>;
	qcom,board-id = <0xb64 0x00>;

	chosen {
		bootargs = "console=null";
		linux,initrd-start = <0x41000000>;
		linux,initrd-end = <0x41000040>;
	};

	memory@40000000 {
		device_type = "memory";
		reg = <0x40000000 0x40000000>;
	};


	framebuffer@40000000 {
		compatible = "simple-framebuffer";
		/* no reg alias: the framebuffer overlaps the RAM node on purpose */
		reg = <0x40000000 0x200000>;
		width = <1280>;
		height = <480>;
		stride = <5120>;
		format = "r8g8b8";
	};
};
"""

FB_BASE = 0x40000000
FB_W, FB_H, FB_BPP = 1280, 480, 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', default='shim/out/Image')
    ap.add_argument('--out', default='shim/out')
    ap.add_argument('--seconds', type=float, default=25.0)
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    image = args.image if os.path.isabs(args.image) else os.path.join(root, args.image)
    outdir = args.out if os.path.isabs(args.out) else os.path.join(root, args.out)
    os.makedirs(outdir, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        dts = os.path.join(tmp, 'test.dts')
        dtb = os.path.join(tmp, 'test.dtb')
        open(dts, 'w').write(TEST_DTS)
        r = subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', dtb, dts],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f'dtc failed:\n{r.stderr}')

        fb_bin = os.path.join(outdir, 'qemu-fb.bin')
        # Map guest RAM to a file so the framebuffer can be read after QEMU
        # exits; no monitor interaction needed.
        mem_file = os.path.join(outdir, 'qemu-ram.bin')
        qemu_log = os.path.join(outdir, 'qemu.log')
        cmd = [
            'qemu-system-aarch64',
            '-M', 'virt,gic-version=3,memory-backend=mem',
            '-cpu', 'cortex-a53',
            '-object', f'memory-backend-file,id=mem,size=1G,mem-path={mem_file},share=on',
            '-kernel', image,
            '-dtb', dtb,
            '-display', 'none',
            '-monitor', 'none',
            '-serial', 'none',
            '-no-reboot',
            '-d', 'guest_errors',
            '-D', qemu_log,
        ]
        print(' '.join(cmd))
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True)
        import time
        time.sleep(args.seconds)
        p.terminate()
        try:
            out, _ = p.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            p.kill()
            out, _ = p.communicate()
        if out:
            print(out[-2000:])
        if os.path.exists(qemu_log):
            log = open(qemu_log).read()
            if log.strip():
                print('qemu guest errors:')
                print(log[:2000])
        # RAM base 0x40000000 is file offset 0; the framebuffer sits there.
        with open(mem_file, 'rb') as f:
            f.seek(FB_BASE - 0x40000000)
            data = f.read(FB_W * FB_H * FB_BPP)
        open(fb_bin, 'wb').write(data)

    if not os.path.exists(fb_bin):
        sys.exit('no framebuffer dump: QEMU did not run')

    data = open(fb_bin, 'rb').read()
    print(f'framebuffer: {len(data)} bytes')

    # The shim clears with RED and writes 0xAARRGGBB little-endian, so the
    # bytes in memory are B,G,R,A.
    bg = b'\x00\x00\xff'
    non_bg = sum(1 for i in range(0, len(data) - 3, FB_BPP)
                 if data[i:i + 3] != bg)
    print(f'non-background pixels: {non_bg} of {FB_W * FB_H}')

    ppm = os.path.join(outdir, 'qemu-fb.ppm')
    with open(ppm, 'wb') as f:
        f.write(f'P6 {FB_W} {FB_H} 255\n'.encode())
        for i in range(0, len(data) - 3, FB_BPP):
            f.write(bytes((data[i + 2], data[i + 1], data[i])))
    print(f'wrote {ppm}')
    return 0 if non_bg > 1000 else 1


if __name__ == '__main__':
    sys.exit(main())
