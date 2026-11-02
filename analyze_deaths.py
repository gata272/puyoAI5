#!/usr/bin/env python3
"""Analyze benchmark decision log: for dying games, at each turn, what was the
best chain-firing safe move available vs what the AI actually chose?
Board encoding in log: string length 84? Let's inspect: preBoard strings in JSON.
Simulator rules mirrored from C++: W=6,H=14, visible 12, group scan y<12,
drop anchor y start 12 (H-2), coords per rotation, top row (y=13) cleared before resolve,
gameOver = height(col2)>=12.
"""
import json, sys
from copy import deepcopy

W,H,VIS=6,14,12

def parse_board(s):
    # Need to know ordering. In chain_benchmark.cpp presumably board serialized row? Let's just load and print lengths, and infer in main.
    return s

def heights(board): # board[x][y]
    hs=[]
    for x in range(W):
        h=0
        for y in range(H):
            if board[x][y]!=0: h=y+1
        hs.append(h)
    return hs

def gravity(board):
    for x in range(W):
        col=[board[x][y] for y in range(H) if board[x][y]!=0]
        for y in range(H):
            board[x][y]= col[y] if y < len(col) else 0

def resolve(board):
    chains=0; erased=0
    while True:
        gravity(board)
        visited=[[False]*H for _ in range(W)]
        groups=[]
        for y in range(VIS):
            for x in range(W):
                if visited[x][y]: continue
                c=board[x][y]
                if c==0 or c==5: continue
                stack=[(x,y)]; visited[x][y]=True; group=[]
                while stack:
                    cx,cy=stack.pop(); group.append((cx,cy))
                    for dx,dy in ((1,0),(-1,0),(0,1),(0,-1)):
                        nx,ny=cx+dx,cy+dy
                        if 0<=nx<W and 0<=ny<VIS and not visited[nx][ny] and board[nx][ny]==c:
                            visited[nx][ny]=True; stack.append((nx,ny))
                if len(group)>=4: groups.append(group)
        if not groups: break
        chains+=1
        cells=[]
        for g in groups: cells+=g; erased+=len(g)
        for (x,y) in cells: board[x][y]=0
        # garbage adjacent
        garb=set()
        for (x,y) in cells:
            for dx,dy in ((1,0),(-1,0),(0,1),(0,-1)):
                nx,ny=x+dx,y+dy
                if 0<=nx<W and 0<=ny<H and board[nx][ny]==5: garb.add((nx,ny))
        for (x,y) in garb: board[x][y]=0
    gravity(board)
    return chains,erased

def coords(x,y,rot):
    sx,sy=x,y
    r=rot&3
    if r==0: sy=y+1
    elif r==1: sx=x-1
    elif r==2: sy=y-1
    else: sx=x+1
    return [(x,y),(sx,sy)]

def can_place(board,pair,x,y,rot):
    for (cx,cy) in coords(x,y,rot):
        if not (0<=cx<W and 0<=cy<H): return False
        if board[cx][cy]!=0: return False
    return True

def find_drop_y(board,pair,x,rot):
    y=H-2
    if not can_place(board,pair,x,y,rot): return -1
    while y>0 and can_place(board,pair,x,y-1,rot):
        y-=1
    return y

def drop(board,pair,move):
    b=[col[:] for col in board]
    x,rot=move
    y=find_drop_y(b,pair,x,rot)
    if y<0: return None
    for i,(cx,cy) in enumerate(coords(x,y,rot)):
        b[cx][cy]= pair[0] if (cx==x and cy==y) else pair[1]
    for xx in range(W): b[xx][H-1]=0
    chains,erased=resolve(b)
    go = heights(b)[2]>=VIS
    allclear = all(b[xx][yy]==0 for xx in range(W) for yy in range(H))
    return b,chains,erased,go,allclear

def legal_moves(board,pair):
    moves=[]
    seen=set()
    for x in range(W):
        for rot in range(4):
            if pair[0]==pair[1] and rot>=2: continue
            y=find_drop_y(board,pair,x,rot)
            if y<0: continue
            moves.append((x,rot))
    return moves

def main():
    data=json.load(open(sys.argv[1]))
    print("summary:", {k:data[k] for k in ("averageMaxChain","gamesOver","averageTurns","atLeast5","atLeast8","atLeast10","atLeast12")})
    log=data["decisionLog"]
    # infer board encoding from first entry
    s=log[0]["turns"][0]["preBoard"]
    print("board string len:", len(s), "sample:", s[:30])
    # chain_benchmark serialization: likely x-major? Let's look at postBoard of turn0: move (0,2) pair main3 sub2 placed at x=0.
    # post string given in earlier stdout: "200000300000..." -> position0='2'(sub?), position6='3'(main?)
    # That suggests index = y*W + x ? At y=0: x0=2, ... ; y=1: x0=3? Wait string: index0='2', index6='3' => stride 6 = one row of width 6, so index=y*W+x, y=0 bottom.
    # rotation2: sub below main. main at (0,1)=3, sub at (0,0)=2. Matches index0=2 (sub at bottom), index6=3 (main above). Good.
    def decode(s):
        b=[[0]*H for _ in range(W)]
        # len should be W*H=84, index=y*W+x
        for idx,ch in enumerate(s):
            y=idx//W; x=idx%W
            if y<H: b[x][y]=int(ch)
        return b
    summaries={g["game"]:g for g in data["gameSummaries"]}
    for game in log:
        gi=game["game"]
        gs=summaries[gi]
        if not gs["gameOver"]: continue
        print(f"\n=== DYING GAME {gi} maxChain={gs['maxChain']} turns={gs['loggedTurns']} ===")
        turns=game["turns"]
        # analyze last 15 turns
        missed=0; total=0
        for t in turns:
            board=decode(t["preBoard"])
            pair=(t["current"]["main"], t["current"]["sub"])
            hs=heights(board); occ=sum(hs)
            moves=legal_moves(board,pair)
            best=None; safe_count=0
            for m in moves:
                r=drop(board,pair,m)
                if r is None: continue
                b,ch,er,go,ac=r
                if not go or ac: safe_count+=1
                cand=(ch,er,m,go,sum(heights(b)),heights(b)[2])
                if best is None or (cand[0],cand[1])>(best[0],best[1]): best=cand
            chosen_chains=t["chains"]
            # what did chosen move erase? score/10 approx not reliable; use chains field + postBoard occ
            post=decode(t["postBoard"]); post_occ=sum(heights(post))
            flag=""
            if best and best[0]>=1 and chosen_chains==0:
                flag=" <-- MISSED FIRE (best chain available but AI built instead)"
                missed+=1
            if t["turn"]>=turns[0]["turn"] and (gs["loggedTurns"]-t["turn"]<=18 or (best and best[0]>=2)):
                print(f"turn {t['turn']:3d} occ={occ:2d} h={hs} safeNextPre={t['safeMoves']} chosen=({t['move']['x']},{t['move']['rotation']}) chosenChains={chosen_chains} postOcc={post_occ} bestAvailChain={best[0] if best else '-'} bestErased={best[1] if best else '-'} bestMove={best[2] if best else '-'}{flag}")
            if gs["loggedTurns"]-t["turn"]<=18: total+=1
        print(f"last-18-turns missed-fire count: {missed} (counted over all turns with best>=1 & chosen 0: see flags)")

if __name__=="__main__":
    main()
