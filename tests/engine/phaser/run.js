const { chromium } = require('playwright');
const http = require('http'), fs = require('fs'), path = require('path');
const root = process.argv[2];
const srv = http.createServer((req, res) => {
  const p = path.join(root, decodeURIComponent(req.url.split('?')[0]));
  fs.readFile(p, (e, d) => { if (e) { res.writeHead(404); return res.end(); }
    const t = p.endsWith('.js') ? 'text/javascript' : p.endsWith('.json') ? 'application/json' : p.endsWith('.png') ? 'image/png' : 'text/html';
    res.writeHead(200, { 'Content-Type': t }); res.end(d); });
}).listen(0, async () => {
  const port = srv.address().port;
  const browser = await chromium.launch();
  for (const page of ['check3.html', 'check4.html']) {
    const p = await browser.newPage();
    p.on('pageerror', e => console.log('pageerror', e.message));
    await p.goto(`http://127.0.0.1:${port}/${page}`);
    await p.waitForFunction(() => window.report, null, { timeout: 15000 });
    console.log(JSON.stringify(await p.evaluate(() => window.report)));
  }
  await browser.close(); srv.close();
});
