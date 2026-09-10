/*
    v86_bench.mjs -- boots a guest under v86, runs a locally built program in
    it and writes a screenshot.  tools\run_v86.ps1 is the front door; this is
    the engine, and it is separate only because v86 is a JavaScript library
    and PowerShell cannot host one.

        node tools\v86_bench.mjs --self-test
        node tools\v86_bench.mjs --guest dos --payload build\dos --run NOTE-DOS.EXE --capture shot.png

    Everything happens in this process.  There is no browser, no window and no
    canvas: v86 is happy to run under Node, and the two things a browser was
    supposedly needed for -- reading the screen and drawing a PNG of it -- are
    done here instead.  Text mode is reconstructed from the character and
    colour pairs the VGA emulation hands its screen adapter, drawn with the
    guest's own 8x16 font read out of VGA plane 2.  Graphics mode is read out
    of the emulator's frame buffer.  That matters beyond politeness: the
    machine this runs on belongs to somebody who is using it, and a bench that
    steals focus to photograph a window can only be run when nobody is home.

    The build under test reaches the guest on a floppy this script generates:
    a 1.44 MB FAT12 image built from a directory, put in whichever floppy
    drive the guest is not booting from, and picked up with no drivers, no
    partition table and no formatting step.  The alternative -- v86's 9p
    filesystem -- was not
    taken: 9p needs a guest driver, and DOS has none.  A hard disk image would
    also work and would lift the 1.44 MB ceiling, at the cost of an MBR, a
    partition entry and a FAT16 formatter; when a build stops fitting on a
    floppy, that is the change to make.
*/

import fs from "fs";
import path from "path";
import zlib from "zlib";
import { fileURLToPath, pathToFileURL } from "url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.dirname(HERE);
const V86 = path.join(HERE, "v86");

// v86 allocates its graphics frame buffer only when it can construct an
// ImageData, which in a browser is how the canvas is fed.  There is no canvas
// here and nothing reads the object back, so the smallest class that holds the
// three fields is enough -- and without it every graphics-mode screenshot
// comes back as a zero-length buffer.
if (typeof globalThis.ImageData === "undefined") {
    globalThis.ImageData = class ImageData {
        constructor(data, width, height) { this.data = data; this.width = width; this.height = height; }
    };
}

/* ---------------------------------------------------------------- arguments */

function parse_args(argv) {
    const o = {
        guest: "dos", payload: null, run: null, keys: "", capture: null,
        text: null, seconds: 6, boot_seconds: 60, start_seconds: 4,
        self_test: false, quiet: false,
    };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        const next = () => argv[++i];
        switch (a) {
            case "--self-test": o.self_test = true; break;
            case "--quiet": o.quiet = true; break;
            case "--guest": o.guest = next(); break;
            case "--payload": o.payload = next(); break;
            case "--run": o.run = next(); break;
            case "--keys": o.keys = next(); break;
            case "--capture": o.capture = next(); break;
            case "--text": o.text = next(); break;
            case "--seconds": o.seconds = Number(next()); break;
            case "--boot-seconds": o.boot_seconds = Number(next()); break;
            case "--start-seconds": o.start_seconds = Number(next()); break;
            case "--image": o.image = next(); break;
            default: throw new Error(`unknown option ${a}`);
        }
    }
    return o;
}

/* ------------------------------------------------------------- FAT12 floppy */

// A 1.44 MB floppy with one file per root directory entry.  Nothing here is
// general: no subdirectories, no long names, no image that is not 1.44 MB.
// That is the whole point -- DOS asks for a BPB it recognises and a FAT that
// agrees with it, and everything past that is a feature we do not need.
export function make_floppy(files) {
    const SECTOR = 512, SECTORS = 2880, FAT_SECTORS = 9, ROOT_ENTRIES = 224;
    const img = Buffer.alloc(SECTOR * SECTORS, 0);

    // Boot sector.  The jump and the OEM name are there because DOS checks
    // them before it will believe the BPB behind them; the image is never
    // booted from, so the code the jump lands on is a halt.
    img.write("\xEB\x3C\x90", 0, "latin1");
    img.write("NOTEBNCH", 3, "latin1");
    img.writeUInt16LE(SECTOR, 11);      // bytes per sector
    img[13] = 1;                        // sectors per cluster
    img.writeUInt16LE(1, 14);           // reserved sectors
    img[16] = 2;                        // FAT count
    img.writeUInt16LE(ROOT_ENTRIES, 17);
    img.writeUInt16LE(SECTORS, 19);
    img[21] = 0xF0;                     // media descriptor: 1.44 MB floppy
    img.writeUInt16LE(FAT_SECTORS, 22);
    img.writeUInt16LE(18, 24);          // sectors per track
    img.writeUInt16LE(2, 26);           // heads
    img[38] = 0x29;                     // extended boot signature
    img.writeUInt32LE(0x4E4F5445, 39);  // volume serial
    img.write("NOTE BENCH ", 43, "latin1");
    img.write("FAT12   ", 54, "latin1");
    img.write("\xF4\xEB\xFD", 62, "latin1");   // hlt; jmp $
    img.writeUInt16LE(0xAA55, 510);

    const fat = Buffer.alloc(SECTOR * FAT_SECTORS, 0);
    fat[0] = 0xF0; fat[1] = 0xFF; fat[2] = 0xFF;
    const set_fat = (n, v) => {
        const off = n + (n >> 1);
        if (n & 1) {
            fat[off] = (fat[off] & 0x0F) | ((v & 0x0F) << 4);
            fat[off + 1] = (v >> 4) & 0xFF;
        } else {
            fat[off] = v & 0xFF;
            fat[off + 1] = (fat[off + 1] & 0xF0) | ((v >> 8) & 0x0F);
        }
    };

    const root_sector = 1 + 2 * FAT_SECTORS;
    const root_sectors = (ROOT_ENTRIES * 32) / SECTOR;
    const data_sector = root_sector + root_sectors;
    const root = img.subarray(root_sector * SECTOR, (root_sector + root_sectors) * SECTOR);

    let cluster = 2, entry = 0;
    for (const file of files) {
        if (entry >= ROOT_ENTRIES) throw new Error("more files than the root directory holds");
        const data = fs.readFileSync(file);
        const clusters = Math.max(1, Math.ceil(data.length / SECTOR));
        if (data_sector + cluster - 2 + clusters > SECTORS) {
            throw new Error(`${path.basename(file)} does not fit on a 1.44 MB floppy`);
        }

        const base = path.basename(file).toUpperCase();
        const dot = base.lastIndexOf(".");
        const stem = (dot > 0 ? base.slice(0, dot) : base).replace(/[^A-Z0-9_~!@#$%^&()'{}-]/g, "_");
        const ext = (dot > 0 ? base.slice(dot + 1) : "").replace(/[^A-Z0-9_]/g, "_");
        if (stem.length > 8 || ext.length > 3) {
            throw new Error(`${base} is not an 8.3 name; DOS cannot see it`);
        }

        const e = root.subarray(entry * 32, entry * 32 + 32);
        e.fill(0x20, 0, 11);
        e.write(stem, 0, "latin1");
        e.write(ext, 8, "latin1");
        e[11] = 0x20;                                   // archive
        e.writeUInt16LE(0x6000, 22);                    // 12:00:00
        e.writeUInt16LE(((2020 - 1980) << 9) | (1 << 5) | 1, 24);
        e.writeUInt16LE(cluster, 26);
        e.writeUInt32LE(data.length, 28);

        data.copy(img, (data_sector + cluster - 2) * SECTOR);
        for (let i = 0; i < clusters; i++) {
            set_fat(cluster + i, i === clusters - 1 ? 0xFFF : cluster + i + 1);
        }
        cluster += clusters;
        entry++;
    }

    fat.copy(img, SECTOR);
    fat.copy(img, SECTOR * (1 + FAT_SECTORS));
    return img;
}

/* ----------------------------------------------------------------- keyboard */

// Scan code set 1, make codes; the break code is the make code with bit 7 set.
// Only the keys tools\run_retro.ps1 spells with {NAME} are here, because those
// are the ones a text editor is driven with and the rest go through as text.
const SCANCODES = {
    ESC: [0x01], ENTER: [0x1C], TAB: [0x0F], BKSP: [0x0E], SPACE: [0x39],
    F1: [0x3B], F2: [0x3C], F3: [0x3D], F4: [0x3E], F5: [0x3F],
    F6: [0x40], F7: [0x41], F8: [0x42], F9: [0x43], F10: [0x44],
    UP: [0xE0, 0x48], DOWN: [0xE0, 0x50], LEFT: [0xE0, 0x4B], RIGHT: [0xE0, 0x4D],
    HOME: [0xE0, 0x47], END: [0xE0, 0x4F], PGUP: [0xE0, 0x49], PGDN: [0xE0, 0x51],
    INS: [0xE0, 0x52], DEL: [0xE0, 0x53],
};

// A US layout, in scan code set 1, in the order the rows sit on the keyboard.
// It is here rather than v86's own keyboard_send_text because that one pushes
// a key's make and break codes into the controller in the same instant, and a
// guest that polls the port rather than draining it on the interrupt sees the
// break and never the make.  Which guests those are is not obvious from
// outside: FreeCOM and vim take v86's keys happily, and the DJGPP build of
// note drops every unshifted character while taking every shifted one --
// because a shifted character sends four codes, and the two in the middle
// survive.  Sending make and break separately, with the emulator running in
// between, takes that whole class of question off the table.
const KEY_ROWS = [
    ["`1234567890-=", "~!@#$%^&*()_+", [0x29, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D]],
    ["qwertyuiop[]", "QWERTYUIOP{}", [0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B]],
    ["asdfghjkl;'", "ASDFGHJKL:\"", [0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28]],
    ["zxcvbnm,./", "ZXCVBNM<>?", [0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35]],
    ["\\", "|", [0x2B]],
    [" ", "", [0x39]],
];

const CHARS = new Map();
for (const [plain, shifted, codes] of KEY_ROWS) {
    for (let i = 0; i < codes.length; i++) {
        CHARS.set(plain[i], [codes[i], false]);
        if (shifted[i]) CHARS.set(shifted[i], [codes[i], true]);
    }
}

const SHIFT = 0x2A;

async function press(emu, make, shift, delay) {
    const half = Math.max(10, delay >> 1);
    if (shift) { emu.keyboard_send_scancodes([SHIFT]); await sleep(half); }
    emu.keyboard_send_scancodes(make);
    await sleep(half);
    emu.keyboard_send_scancodes(make.map(c => (c === 0xE0 ? 0xE0 : c | 0x80)));
    await sleep(half);
    if (shift) { emu.keyboard_send_scancodes([SHIFT | 0x80]); await sleep(half); }
}

// Keys go in one at a time with the emulator running in between: sending the
// lot in one go overruns the BIOS's sixteen-byte buffer and the guest beeps
// and drops the rest.
async function send_keys(emu, keys, delay = 60) {
    for (const piece of keys.split(/(\{[A-Z0-9]+\})/)) {
        if (!piece) continue;
        const m = /^\{([A-Z0-9]+)\}$/.exec(piece);
        if (m) {
            const code = SCANCODES[m[1]];
            if (!code) throw new Error(`unknown key {${m[1]}}`);
            await press(emu, code, false, delay);
        } else {
            for (const ch of piece) {
                const key = CHARS.get(ch);
                if (!key) throw new Error(`no key on a US keyboard types '${ch}'`);
                await press(emu, [key[0]], key[1], delay);
            }
        }
    }
}

const sleep = ms => new Promise(r => setTimeout(r, ms));

/* ------------------------------------------------------------------ capture */

// The 8x16 VGA font the guest is actually using lives in VGA plane 2, 32 bytes
// per glyph.  Reading it from there rather than shipping a font means a guest
// that loads its own character set is drawn with it.  A guest that has not got
// round to loading one yet leaves plane 2 blank, so the video BIOS image is
// the fallback: it carries the same font, found by looking for the glyph for
// character 1, which is the smiling face every IBM font since 1981 has had.
function read_font(vga) {
    const glyphs = new Uint8Array(256 * 16);
    const plane2 = vga && vga.plane2;
    let blank = true;
    if (plane2 && plane2.length >= 256 * 32) {
        for (let c = 0; c < 256; c++) {
            for (let y = 0; y < 16; y++) {
                const b = plane2[c * 32 + y];
                glyphs[c * 16 + y] = b;
                if (b) blank = false;
            }
        }
    }
    if (!blank) return glyphs;

    const rom = fs.readFileSync(path.join(V86, "bios", "vgabios.bin"));
    const smiley = Buffer.from([0, 0, 0x7E, 0x81, 0xA5, 0x81, 0x81, 0xBD, 0x99, 0x81, 0x81, 0x7E, 0, 0, 0, 0]);
    const at = rom.indexOf(smiley);
    if (at < 16) throw new Error("no 8x16 font in VGA plane 2 or in vgabios.bin");
    return new Uint8Array(rom.subarray(at - 16, at - 16 + 256 * 16));
}

function render_text(screen, font) {
    const { cols, rows, cells } = screen;
    const w = cols * 8, h = rows * 16;
    const px = Buffer.alloc(w * h * 3);
    for (let r = 0; r < rows; r++) {
        for (let c = 0; c < cols; c++) {
            const cell = cells[r * cols + c];
            const glyph = (cell.chr & 0xFF) * 16;
            for (let y = 0; y < 16; y++) {
                const bits = font[glyph + y];
                let o = ((r * 16 + y) * w + c * 8) * 3;
                for (let x = 0; x < 8; x++, o += 3) {
                    const rgb = (bits >> (7 - x)) & 1 ? cell.fg : cell.bg;
                    px[o] = (rgb >> 16) & 0xFF;
                    px[o + 1] = (rgb >> 8) & 0xFF;
                    px[o + 2] = rgb & 0xFF;
                }
            }
        }
    }
    return { width: w, height: h, pixels: px };
}

// In graphics mode the emulator keeps a finished frame in wasm memory, four
// bytes per pixel, once something asks it to fill one.  Nothing does here --
// that is a canvas's job in a browser -- so the fill is asked for by hand.
// The frame is as wide as the emulator last allocated for, which is not the
// same as the mode's width -- the buffer is kept across mode changes and only
// grown -- so the visible rectangle is copied out of it a row at a time.
function render_graphics(vga, cpu) {
    vga.screen_fill_buffer();
    const w = vga.screen_width, h = vga.screen_height, stride = vga.virtual_width;
    if (!w || !h || !stride || !vga.dest_buffet_offset) return null;
    const src = new Uint8Array(cpu.wasm_memory.buffer, vga.dest_buffet_offset,
                               stride * vga.virtual_height * 4);
    const px = Buffer.alloc(w * h * 3);
    for (let y = 0, o = 0; y < h; y++) {
        for (let x = 0; x < w; x++, o += 3) {
            const i = (y * stride + x) * 4;
            px[o] = src[i];
            px[o + 1] = src[i + 1];
            px[o + 2] = src[i + 2];
        }
    }
    return { width: w, height: h, pixels: px };
}

// A PNG of 24-bit pixels is a zlib stream of rows, each prefixed with a filter
// byte, wrapped in four chunks.  Node has the zlib; the rest is bookkeeping,
// and it is cheaper than a dependency.
function write_png(file, { width, height, pixels }) {
    const raw = Buffer.alloc((width * 3 + 1) * height);
    for (let y = 0; y < height; y++) {
        raw[y * (width * 3 + 1)] = 0;
        pixels.copy(raw, y * (width * 3 + 1) + 1, y * width * 3, (y + 1) * width * 3);
    }
    const chunk = (type, data) => {
        const b = Buffer.alloc(8 + data.length + 4);
        b.writeUInt32BE(data.length, 0);
        b.write(type, 4, "latin1");
        data.copy(b, 8);
        b.writeUInt32BE(crc32(b.subarray(4, 8 + data.length)) >>> 0, 8 + data.length);
        return b;
    };
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(width, 0);
    ihdr.writeUInt32BE(height, 4);
    ihdr[8] = 8; ihdr[9] = 2;
    fs.mkdirSync(path.dirname(path.resolve(file)), { recursive: true });
    fs.writeFileSync(file, Buffer.concat([
        Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
        chunk("IHDR", ihdr),
        chunk("IDAT", zlib.deflateSync(raw, { level: 9 })),
        chunk("IEND", Buffer.alloc(0)),
    ]));
}

const CRC_TABLE = (() => {
    const t = new Int32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
        t[n] = c;
    }
    return t;
})();

function crc32(buf) {
    let c = -1;
    for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xFF] ^ (c >>> 8);
    return c ^ -1;
}

/* ---------------------------------------------------------------- the guest */

// The screen adapter is the only thing the VGA emulation tells about characters
// and their colours, and under Node it is a stub that throws the lot away.
// Wrapping its methods gets a full 80x25 of characters plus foreground and
// background out of it without touching VGA internals -- which is the same
// information a browser would put on a canvas.
function watch_screen(emu) {
    const adapter = emu.screen_adapter;
    const screen = { cols: 80, rows: 25, cells: [], graphical: false };
    const resize = (cols, rows) => {
        screen.cols = cols; screen.rows = rows;
        screen.cells = Array.from({ length: cols * rows }, () => ({ chr: 32, fg: 0xAAAAAA, bg: 0 }));
    };
    resize(80, 25);

    const wrap = (name, fn) => {
        const original = adapter[name] && adapter[name].bind(adapter);
        adapter[name] = (...args) => { fn(...args); if (original) return original(...args); };
    };
    wrap("set_size_text", (cols, rows) => resize(cols, rows));
    wrap("set_mode", graphical => { screen.graphical = !!graphical; });
    wrap("clear_screen", () => resize(screen.cols, screen.rows));
    wrap("put_char", (row, col, chr, blinking, bg, fg) => {
        const cell = screen.cells[row * screen.cols + col];
        if (cell) { cell.chr = chr; cell.bg = bg; cell.fg = fg; }
    });
    return screen;
}

function text_of(screen) {
    const lines = [];
    for (let r = 0; r < screen.rows; r++) {
        let line = "";
        for (let c = 0; c < screen.cols; c++) {
            const chr = screen.cells[r * screen.cols + c].chr;
            line += chr >= 32 && chr < 127 ? String.fromCharCode(chr) : chr === 0 ? " " : ".";
        }
        lines.push(line.replace(/\s+$/, ""));
    }
    return lines;
}

async function boot(options) {
    // A Windows path is not a URL, and Node's ESM loader will only take one.
    const { V86: V86Starter } = await import(pathToFileURL(path.join(V86, "build", "libv86.mjs")).href);
    const buffer = p => ({ buffer: fs.readFileSync(p).buffer });

    const config = {
        bios: buffer(path.join(V86, "bios", "seabios.bin")),
        vga_bios: buffer(path.join(V86, "bios", "vgabios.bin")),
        wasm_path: path.join(V86, "build", "v86.wasm"),
        memory_size: 64 * 1024 * 1024,
        vga_memory_size: 8 * 1024 * 1024,
        screen: {},                 // no container: the headless adapter
        disable_speaker: true,
        disable_mouse: true,
        autostart: true,
    };

    if (options.guest === "dos") {
        config.fda = buffer(path.join(V86, "images", "freedos722.img"));
        config.boot_order = 0x321;  // floppy first
    } else {
        // A Windows guest is a hard disk, and it is the user's to supply --
        // see the header of tools\run_v86.ps1.
        const image = options.image;
        if (!image) throw new Error(`no disk image for guest '${options.guest}'`);
        if (!fs.existsSync(image)) throw new Error(`no disk image at ${image}`);
        config.hda = buffer(image);
        config.boot_order = 0x132;  // hard disk first
    }

    // The payload floppy goes in whichever drive the guest is not booting
    // from: B: behind the FreeDOS boot floppy, A: behind a hard disk.
    if (options.floppy) {
        const image = { buffer: options.floppy.buffer };
        if (config.fda) config.fdb = image; else config.fda = image;
    }

    const emu = new V86Starter(config);
    await new Promise(resolve => emu.add_listener("emulator-ready", resolve));
    return emu;
}

/* -------------------------------------------------------------------- main */

async function main() {
    const options = parse_args(process.argv.slice(2));
    const say = (...a) => { if (!options.quiet) console.log(...a); };

    if (options.self_test) {
        const emu = await boot({ guest: "dos" });
        const screen = watch_screen(emu);
        const ok = await emu.wait_until_vga_screen_contains(/^[A-Z]:\\>/, { timeout_msec: 60000 });
        emu.destroy();
        if (!ok) {
            console.error("FreeDOS did not reach a prompt:");
            console.error(text_of(screen).join("\n"));
            process.exit(1);
        }
        say("v86 booted FreeDOS to a prompt.");
        process.exit(0);
    }

    // Everything the payload directory holds goes on the floppy.  A directory
    // rather than a file list because a DOS build is rarely one file: the
    // DJGPP editor needs CWSDPMI.EXE beside it or it exits with nothing on
    // screen but "no DPMI".
    let floppy = null;
    if (options.payload) {
        const p = path.resolve(ROOT, options.payload);
        // The leavings of the build are not part of it, and a build directory
        // is full of them.  Object files and link maps are dropped by
        // extension; anything whose name DOS could not read is dropped too,
        // and said out loud rather than silently, in case it was the point.
        const litter = /\.(obj|o|map|lst|sym|err|swp|pdb|ilk)$/i;
        const listed = fs.statSync(p).isDirectory()
            ? fs.readdirSync(p).map(f => path.join(p, f)).filter(f => fs.statSync(f).isFile())
            : [p];
        const files = [];
        for (const f of listed) {
            const name = path.basename(f);
            if (litter.test(name)) continue;
            if (!/^[^.]{1,8}(\.[^.]{1,3})?$/.test(name)) {
                say(`Leaving out ${name}: DOS cannot see a name that is not 8.3.`);
                continue;
            }
            files.push(f);
        }
        if (!files.length) throw new Error(`nothing to put on the floppy in ${p}`);
        floppy = make_floppy(files);
        say(`Payload floppy: ${files.length} file(s), ${files.map(f => path.basename(f)).join(" ")}`);
    }

    const emu = await boot({ ...options, floppy });
    const screen = watch_screen(emu);

    if (options.guest === "dos") {
        // A DOS guest announces it is ready by drawing a prompt, so there is
        // no need to guess how long the boot takes.  Nothing else does: a
        // Windows guest is finished booting when it feels like it, which is
        // what --boot-seconds is for.
        const prompt = await emu.wait_until_vga_screen_contains(/^[A-Z]:\\>/, { timeout_msec: options.boot_seconds * 1000 });
        if (!prompt) {
            emu.destroy();
            console.error("The guest never reached a command prompt:");
            console.error(text_of(screen).join("\n"));
            process.exit(1);
        }
    } else {
        await sleep(options.boot_seconds * 1000);
    }
    say(`Guest is up after ${emu.get_instruction_counter()} instructions.`);

    if (floppy && options.guest === "dos") {
        await send_keys(emu, "B:{ENTER}", 40);
        await sleep(500);
    }
    if (options.run) {
        await send_keys(emu, options.run + "{ENTER}", 40);
        // Keys typed while the program is still loading go into the BIOS's
        // sixteen-byte buffer, and a program that flushes it on startup --
        // most full-screen editors do -- loses them.  A DJGPP build loading
        // its DPMI host off a floppy takes seconds, not milliseconds.
        await sleep(options.start_seconds * 1000);
    }
    if (options.keys) await send_keys(emu, options.keys);

    await sleep(options.seconds * 1000);

    const vga = emu.v86.cpu.devices.vga;
    let image = null;
    if (screen.graphical || (vga && vga.graphical_mode)) {
        image = render_graphics(vga, emu.v86.cpu);
        if (!image) say("The guest is in graphics mode but the emulator has no frame to give.");
    }
    if (!image) image = render_text(screen, read_font(vga));

    const lines = text_of(screen);
    if (options.text) {
        fs.mkdirSync(path.dirname(path.resolve(options.text)), { recursive: true });
        fs.writeFileSync(options.text, lines.join("\n") + "\n", "latin1");
    }
    if (options.capture) {
        write_png(options.capture, image);
        say(`Captured ${options.capture} (${image.width}x${image.height})`);
    }
    if (!options.capture && !options.text) say(lines.join("\n"));

    emu.destroy();
    process.exit(0);
}

// Only when run as a program: make_floppy is exported so a one-off script can
// build an image without booting anything.
if (process.argv[1] && pathToFileURL(path.resolve(process.argv[1])).href === import.meta.url) {
    main().catch(e => {
        console.error(e.stack || String(e));
        process.exit(1);
    });
}
