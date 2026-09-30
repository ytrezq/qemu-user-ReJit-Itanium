import sys, collections
a = open(sys.argv[1]).read().split('\n'); b = open(sys.argv[2]).read().split('\n')
bad = collections.Counter(); ex = {}
for x, y in zip(a, b):
    if x != y:
        k = x.split()[0] if x else '?'
        bad[k] += 1
        if k not in ex: ex[k] = (x, y)
print("lines", len(a), len(b), "mismatches", sum(bad.values()))
for k, v in bad.most_common(): print(" ", k, v, "\n    ref:", ex[k][0], "\n    jit:", ex[k][1])
