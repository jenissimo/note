/* serve_v86.mjs — the same guests run_v86.ps1 boots, in a browser you drive.
 *
 * run_v86.ps1 exists to prove things without a human: it runs v86 under Node,
 * types at the guest and writes a PNG.  This is the other half of the same
 * idea.  v86 is a browser emulator to begin with, so pointing a browser at it
 * costs a static page and a file server -- and then the guest is yours: you
 * click around in it, run the editor, open menus, and nothing has to be
 * scripted in advance.
 *
 * The file server is here rather than being any of the usual one-liners
 * because of one requirement: v86 loads a large disk image with `async: true`,
 * which means HTTP range requests for the 256 KB it needs at a time.  A server
 * that ignores Range hands the browser the whole image instead -- half a
 * gigabyte into a tab, before the BIOS has run.
 *
 * The build under test reaches the guest exactly as it does under Node: the
 * payload directory becomes a FAT12 floppy, built by the same make_floppy the
 * bench uses, so there is one implementation of that and not two.
 */

import fs from "node:fs";
import http from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { make_floppy } from "./v86_bench.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const V86 = path.join(HERE, "v86");

/* ---- what to boot ------------------------------------------------------ */

const args = process.argv.slice(2);
const target = args[0] === "dos" || args[0] === "win95" ? args[0] : "win95";
const opt = (name, fallback) => {
    const i = args.indexOf("-" + name);
    return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};

const port = parseInt(opt("Port", "8086"), 10);
let payload = opt("Payload", "");
if (!payload) {
    const dos16 = path.join(HERE, "..", "build", "dos16");
    const dos = path.join(HERE, "..", "build", "dos");
    payload = fs.existsSync(dos16) ? dos16 : dos;
}

const win95 = process.env.NOTE_V86_WIN95 ||
              path.join(V86, "images", "win95.img");

if (target === "win95" && !fs.existsSync(win95)) {
    console.error(`No Windows 95 image at ${win95}.`);
    console.error("Set NOTE_V86_WIN95, or drop a raw disk image there.");
    process.exit(1);
}

/* ---- the payload floppy ------------------------------------------------ */

/* Only what DOS can name and only what is not build litter, the same rule the
 * bench applies -- a floppy full of .obj files helps nobody. */
const LITTER = /\.(obj|o|map|lst|sym|err|swp|pdb|ilk|lz|res|png|txt)$/i;
const files = [];
if (fs.existsSync(payload)) {
    for (const name of fs.readdirSync(payload)) {
        const full = path.join(payload, name);
        if (!fs.statSync(full).isFile() || LITTER.test(name)) continue;
        if (!/^[^.]{1,8}(\.[^.]{1,3})?$/.test(name)) {
            console.log(`  leaving out ${name}: DOS cannot see that name.`);
            continue;
        }
        files.push(full);
    }
}
if (!files.length) throw new Error(`nothing to put on the floppy in ${payload}`);

/* make_floppy takes paths, and it is the bench's -- one FAT12 writer, not two. */
const floppy = make_floppy(files);
console.log(`Payload floppy: ${files.length} file(s), ` +
            files.map(f => path.basename(f)).join(" "));

/* ---- the page ---------------------------------------------------------- */

const hda = target === "win95"
    ? `hda: { url: "/disk.img", async: true, size: ${fs.statSync(win95).size} },`
    : "";
const fda = target === "win95"
    ? `fda: { url: "/payload.img" },`
    : `fda: { url: "/boot.img" }, fdb: { url: "/payload.img" },`;

/* The payload floppy is data, not a boot disk -- make_floppy writes a boot
 * sector whose jump lands on a halt, because nothing was ever meant to boot
 * from it.  Left to itself SeaBIOS prefers the floppy, finds that halt and
 * stops there, which looks exactly like the emulator hanging.  So the order
 * is stated: the hard disk first when there is one, the FreeDOS floppy first
 * when there is not. */
const boot = target === "win95" ? "0x132" : "0x321";

const page = `<!doctype html>
<title>note under ${target === "win95" ? "Windows 95" : "FreeDOS"}</title>
<style>
  body { margin: 0; background: #1a1a1a; color: #ddd;
         font: 13px/1.5 Consolas, monospace; }
  #bar { padding: 8px 12px; }
  #screen { display: inline-block; background: #000; }
  #screen div { white-space: pre; font: 14px/1.05 monospace; }
  canvas { display: block; }
</style>
<div id=bar>note — ${target} — the payload floppy is <b>${target === "win95" ? "A:" : "B:"}</b></div>
<div id=screen><div></div><canvas></canvas></div>
<script type="module">
import { V86 } from "/v86/build/libv86.mjs";
new V86({
    wasm_path: "/v86/build/v86.wasm",
    bios:      { url: "/v86/bios/seabios.bin" },
    vga_bios:  { url: "/v86/bios/vgabios.bin" },
    ${hda}
    ${fda}
    boot_order: ${boot},
    memory_size: 64 * 1024 * 1024,
    vga_memory_size: 8 * 1024 * 1024,
    screen_container: document.getElementById("screen"),
    autostart: true,
});
</script>
`;

/* ---- the server -------------------------------------------------------- */

const TYPES = { ".mjs": "text/javascript", ".js": "text/javascript",
                ".wasm": "application/wasm", ".html": "text/html" };

/* Range matters here: without it the browser is handed the whole disk image
 * rather than the piece v86 asked for. */
function send(req, res, file, type) {
    const size = fs.statSync(file).size;
    const range = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || "");

    if (!range) {
        res.writeHead(200, { "Content-Type": type, "Content-Length": size,
                             "Accept-Ranges": "bytes" });
        return fs.createReadStream(file).pipe(res);
    }

    const start = range[1] ? parseInt(range[1], 10) : 0;
    const end = range[2] ? parseInt(range[2], 10) : size - 1;
    res.writeHead(206, {
        "Content-Type": type,
        "Content-Range": `bytes ${start}-${end}/${size}`,
        "Content-Length": end - start + 1,
        "Accept-Ranges": "bytes",
    });
    fs.createReadStream(file, { start, end }).pipe(res);
}

http.createServer((req, res) => {
    const url = decodeURIComponent(req.url.split("?")[0]);

    if (url === "/" || url === "/index.html") {
        res.writeHead(200, { "Content-Type": "text/html" });
        return res.end(page);
    }
    if (url === "/payload.img") {
        /* Never from the cache: this image is rebuilt between reloads and a
         * stale one looks exactly like a change that did not take. */
        res.writeHead(200, { "Content-Type": "application/octet-stream",
                             "Cache-Control": "no-store, must-revalidate",
                             "Content-Length": floppy.length });
        return res.end(Buffer.from(floppy));
    }
    if (url === "/boot.img") {
        return send(req, res, path.join(V86, "images", "freedos722.img"),
                    "application/octet-stream");
    }
    if (url === "/disk.img") {
        return send(req, res, win95, "application/octet-stream");
    }
    if (url.startsWith("/v86/")) {
        /* Only from the vendored tree, and only files that are in it. */
        const file = path.join(V86, url.slice("/v86/".length));
        if (!path.resolve(file).startsWith(path.resolve(V86)) ||
            !fs.existsSync(file)) {
            res.writeHead(404); return res.end("no");
        }
        return send(req, res, file, TYPES[path.extname(file)] ||
                                    "application/octet-stream");
    }
    res.writeHead(404);
    res.end("no");
}).listen(port, "127.0.0.1", () => {
    console.log(`\n  http://127.0.0.1:${port}/   — open this\n`);
    console.log("  Ctrl+C here when you are done.");
});
