"""Frozen tokenisation for the two free baselines.

Both baselines are the measuring stick, not the system. Nothing here may import
from realontext's own code, and nothing here gets tuned after stage 0.
"""
import re

IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
CAMEL_RE = re.compile(r"[A-Z]+(?=[A-Z][a-z])|[A-Z][a-z0-9]*|[a-z0-9]+|_")

EN_STOP = set("""
a about above after again against all am an and any are aren as at be because been
before being below between both but by can cannot could couldn did didn do does
doesn doing don down during each few for from further had hadn has hasn have haven
having he her here hers herself him himself his how i if in into is isn it its
itself just me more most must mustn my myself no nor not now of off on once only
or other ought our ours ourselves out over own same shan she should shouldn so
some such than that the their theirs them themselves then there these they this
those through to too under until up very was wasn we were weren what when where
which while who whom why with won would wouldn you your yours yourself yourselves
also get got make made use used using see seem like want need issue problem bug
error fail failed failing expect expected actual behaviour behavior version
reproduce steps example test tests case cases code line lines file files run
running works work working when trying tried please thanks thank hello hi
""".split())

# Language keywords: high frequency, zero locating power.
KEYWORDS = set("""
if else for while do break continue return goto switch case default try catch
finally throw throws new delete this self super null nil none true false void
int long short char float double bool boolean string str byte bytes const let var
static public private protected final abstract virtual override extern inline
struct class enum union interface trait impl fn func def lambda async await yield
import export from package module use using namespace typedef template typename
type where match mut ref pub crate mod dyn unsafe macro println printf print
sizeof static_cast dynamic_cast reinterpret_cast auto register volatile
synchronized transient instanceof extends implements throws assert with as pass
raise except elif not and or in is del global nonlocal
""".split())

STOP = EN_STOP | KEYWORDS


def grep_terms(text, min_len=4):
    """Rule 1-2 of the grep baseline: identifiers >= 4 chars, minus the stop tables."""
    seen, out = set(), []
    for m in IDENT_RE.finditer(text):
        w = m.group(0)
        if len(w) < min_len or w.lower() in STOP:
            continue
        if w in seen:
            continue
        seen.add(w)
        out.append(w)
    return out


def bm25_terms(text, min_len=2):
    """camelCase / snake_case split, original form kept alongside the pieces."""
    out = []
    for m in IDENT_RE.finditer(text):
        w = m.group(0)
        low = w.lower()
        if len(low) >= min_len and low not in STOP:
            out.append(low)
        pieces = [p.lower() for p in CAMEL_RE.findall(w) if p != "_"]
        if len(pieces) > 1:
            for p in pieces:
                if len(p) >= min_len and p not in STOP:
                    out.append(p)
    return out
