// Call the running examples through the official JavaScript client (@gradio/client).
//   npm install @gradio/client
//   node tests/js_client_test.mjs hello|blocks|sepia|login [some.png]
// NODE_PATH-style resolution: run with the client installed next to this file or set GRADIO_CLIENT_DIR.
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { pathToFileURL } from "node:url";

const require = createRequire(path.join(process.env.GRADIO_CLIENT_DIR || process.cwd(), "x.js"));
const { Client, handle_file } = await import(pathToFileURL(require.resolve("@gradio/client")).href);

const URL = process.env.GRADIO_URL || "http://127.0.0.1:7860/";
const [which, png] = process.argv.slice(2);
const assert = (c, m) => { if (!c) throw new Error("assert: " + m); };

if (which === "hello") {
  const app = await Client.connect(URL);
  const r = await app.predict("/predict", { name: "JS", intensity: 2 });
  assert(r.data[0] === "Hello, JS!!", JSON.stringify(r.data));
  console.log("js hello ok:", r.data[0]);
} else if (which === "blocks") {
  const app = await Client.connect(URL);
  const seen = [];
  const job = app.submit("/stream", ["js", 3]);
  for await (const msg of job) if (msg.type === "data") seen.push(msg.data[0]);
  assert(seen.at(-1) === "1: js\n2: js\n3: js\n", JSON.stringify(seen));
  console.log("js blocks ok:", seen.length, "updates");
} else if (which === "sepia") {
  const app = await Client.connect(URL);
  const blob = new Blob([readFileSync(png)], { type: "image/png" });
  const r = await app.predict("/predict", [handle_file(blob), 40, "jpeg"]);
  const [img, size] = r.data;
  assert(img && img.url && size.includes(" x "), JSON.stringify(r.data));
  const back = await fetch(img.url);
  assert(back.ok && (await back.arrayBuffer()).byteLength > 1000, "download " + img.url);
  console.log("js sepia ok:", size, img.url.slice(0, 60) + "...");
} else if (which === "login") {
  let refused = false;
  try { await Client.connect(URL); } catch { refused = true; }
  assert(refused, "connect without auth should fail");
  const app = await Client.connect(URL, { auth: ["admin", "secret"] });
  const r = await app.predict("/predict", ["JS"]);
  assert(r.data[0].startsWith("Hello, JS! (logged in as admin"), r.data[0]);
  console.log("js login ok:", r.data[0]);
} else {
  throw new Error("unknown test " + which);
}
process.exit(0);
