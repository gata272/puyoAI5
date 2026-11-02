#!/usr/bin/env python3
import json,sys
sys.path.insert(0,'.')
from analyze_deaths import drop, heights, W,H

data=json.load(open('bench_orig_10.json'))
def decode(s):
    b=[[0]*H for _ in range(W)]
    for idx,ch in enumerate(s):
        y=idx//W; x=idx%W
        if y<H: b[x][y]=int(ch)
    return b
def encode(b):
    out=[]
    for y in range(H):
        for x in range(W):
            out.append(str(b[x][y]))
    return ''.join(out)

total=0; match_board=0; match_chains=0; mismatch=[]
for game in data["decisionLog"]:
    for t in game["turns"]:
        board=decode(t["preBoard"])
        pair=(t["current"]["main"], t["current"]["sub"])
        mv=(t["move"]["x"], t["move"]["rotation"])
        if not t["move"]["valid"]: continue
        r=drop(board,pair,mv)
        total+=1
        if r is None:
            mismatch.append((game["game"],t["turn"],"dropNone",mv,pair)); continue
        b,ch,er,go,ac=r
        if encode(b)==t["postBoard"]: match_board+=1
        else: mismatch.append((game["game"],t["turn"],"boardDiff",mv,pair))
        if ch==t["chains"]: match_chains+=1
        elif len(mismatch)<20: mismatch.append((game["game"],t["turn"],f"chains py={ch} cpp={t['chains']}",mv,pair))
print(f"total={total} boardMatch={match_board} chainMatch={match_chains}")
for m in mismatch[:20]: print(m)
