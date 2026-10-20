import json
S='src/'
frag=open('glass_frag.glsl',encoding='utf-8').read()
parts=['const GLASS_FRAG_SRC = '+json.dumps(frag)+';']
for f in ['gl.js','art.js','paint.js','island.js','sound.js','flow.js','demo.js','figs.js','switch.js']: parts.append(open(S+f,encoding='utf-8').read())
parts.append("window.PB_STYLE = window.PB_STYLE || 'gummy';\nconst safe = (n, f) => { try { f(); } catch (e) { console.error(n, e); if (n === 'demo') { const d = document.getElementById('demo'); if (d) showDemoError(d, e); } } };\nsafe('switch', initStyleSwitch);\nsafe('demo', initDemo);\nsafe('figs', initFigures);")
open('assets/app.js','w',encoding='utf-8').write('/* Pocket Board site — flowing background + interactive demo */\n(() => {\n'+'\n'.join(parts)+'\n})();\n')
print('ok')
