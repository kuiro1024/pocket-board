// Pocket Board サイトの訪問数カウンター（Cloudflare Workers + D1）
//   POST /visit … 数を 1 増やして、増やした後の数を返す（サイトを開いたとき）
//   GET  /visit … 今の数を返すだけ
//   POST /quack?add=k … みんながフッターのアヒルを押した回数に k（最大20）を足して、合計を返す
//   GET  /quack … その合計を返すだけ
const ALLOWED = ['https://kuiro1024.github.io', 'http://localhost:8000', 'http://127.0.0.1:8000'];

export default {
  async fetch(req, env) {
    const origin = req.headers.get('Origin') || '';
    const cors = {
      'Access-Control-Allow-Origin': ALLOWED.includes(origin) ? origin : ALLOWED[0],
      'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
      'Vary': 'Origin',
    };
    const json = (body, status = 200) => new Response(JSON.stringify(body), {status, headers: {...cors, 'Content-Type': 'application/json', 'Cache-Control': 'no-store'}});
    if (req.method === 'OPTIONS') return new Response(null, {headers: cors});
    const url = new URL(req.url);
    if (url.pathname === '/quack') {
      if (req.method === 'POST') {
        if (!ALLOWED.includes(origin)) return json({error: 'forbidden'}, 403);
        const add = Math.max(1, Math.min(20, parseInt(url.searchParams.get('add'), 10) || 1));   // まとめて送る（連打で書き込みを使い切らないように）
        const row = await env.DB.prepare('INSERT INTO counter (id, n) VALUES (2, ?1) ON CONFLICT(id) DO UPDATE SET n = n + ?1 RETURNING n').bind(add).first();
        return json({n: row ? row.n : 0});
      }
      if (req.method === 'GET') {
        const row = await env.DB.prepare('SELECT n FROM counter WHERE id = 2').first();
        return json({n: row ? row.n : 0});
      }
      return json({error: 'method not allowed'}, 405);
    }
    if (url.pathname !== '/visit') return json({error: 'not found'}, 404);

    if (req.method === 'POST') {
      if (!ALLOWED.includes(origin)) return json({error: 'forbidden'}, 403);   // 他のサイトから数を増やされないように
      const row = await env.DB.prepare('UPDATE counter SET n = n + 1 WHERE id = 1 RETURNING n').first();
      return json({n: row ? row.n : 0});
    }
    if (req.method === 'GET') {
      const row = await env.DB.prepare('SELECT n FROM counter WHERE id = 1').first();
      return json({n: row ? row.n : 0});
    }
    return json({error: 'method not allowed'}, 405);
  },
};
