import io
p = 'x64/Release/a/READ ME - DLSS Neural Rendering.txt'
t = io.open(p, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in t else '\n'
anchor = '  HUD detection      Keeps the model off the interface'
assert anchor in t
new = ('  Proxy curve        In the split (and the DX11 bridge) the model sees the frame before the game\'s' + nl +
       '                     tonemapper, compressed through a curve of ours. "Match the game\'s tonemapper"' + nl +
       '                     learns the game\'s own curve by comparing the linear frame with the finished one,' + nl +
       '                     so the model is shown the game\'s real contrast and shadow depth instead of a' + nl +
       '                     generic guess -- most of the detail the split used to lose comes back. Needs an' + nl +
       '                     SDR display output to learn from; Reinhard until the first measurement lands.' + nl + nl)
t = t.replace(anchor, new + anchor, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(t)
print('readme updated')
