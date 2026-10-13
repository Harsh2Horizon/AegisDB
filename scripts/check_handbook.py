#!/usr/bin/env python3
"""Check offline links, unique IDs, section coverage and SVG references."""
from pathlib import Path
from html.parser import HTMLParser
import re

root=Path(__file__).resolve().parents[1]
class Check(HTMLParser):
    def __init__(self):
        super().__init__();self.ids=set();self.links=[];self.sections=0;self.resources=[]
    def handle_starttag(self,tag,attrs):
        a=dict(attrs)
        if 'id' in a:
            assert a['id'] not in self.ids, ('duplicate id',a['id'])
            self.ids.add(a['id'])
        if tag=='section':self.sections+=1
        if tag=='a':self.links.append(a.get('href',''))
        if tag in ('script','img','link'):
            resource=a.get('src',a.get('href'))
            if resource:self.resources.append(resource)

text=(root/'docs/architecture.html').read_text();check=Check();check.feed(text)
assert check.sections==27,check.sections
for link in check.links:
    if link.startswith('#'):assert link[1:] in check.ids,link
    else:assert (root/'docs'/link).resolve().is_file(),link
for ref in re.findall(r'url\(#([^)]+)\)',text):assert ref in check.ids,ref
assert not any(re.match(r'https?://',r) for r in check.resources),'external runtime resource'
print(f'PASS: 27 chapters, {len(check.links)} links, {len(check.ids)} unique IDs, offline resources')
