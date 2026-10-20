p = open('page.html', encoding='utf-8').read()
# INLINE_APP
p = p.replace('<script src="assets/app.js"></script>', '<script>' + open('assets/app.js', encoding='utf-8').read() + '</script>')
head = '''<!doctype html>
<html lang="ja" class="lang-ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="description" content="Pocket Board — ショートカットで呼び出せる、動画・画像・リンクをなんでも置けるWindows用のガラスのボード。無料・オープンソース。">
<meta property="og:title" content="Pocket Board">
<meta property="og:description" content="動画も、画像も、サイトも。すべてをひとつのキャンバスに。Windows用・無料・オープンソース。">
<meta property="og:image" content="https://kuiro1024.github.io/pocket-board/assets/og.jpg">
<meta property="og:url" content="https://kuiro1024.github.io/pocket-board/">
<meta property="og:type" content="website">
<meta name="twitter:card" content="summary_large_image">
<link rel="icon" href="assets/favicon.png">
<link rel="apple-touch-icon" href="assets/apple-touch-icon.png">
<style>:root{padding-top:env(safe-area-inset-top,0px);padding-bottom:env(safe-area-inset-bottom,0px)}body{margin:0}img{max-width:100%}[hidden]{display:none!important}</style>
'''
p = p.replace('<meta name="claude:viewport-fit-sides" content="cover">\n', '')
# <title> はhead内へ
title, rest = p.split('\n', 1)
i = rest.index('</style>') + len('</style>')
out = head + title + '\n' + rest[:i] + '\n</head>\n<body>\n' + rest[i:] + '\n</body>\n</html>\n'
open('index.html', 'w', encoding='utf-8').write(out)
print('ok')
