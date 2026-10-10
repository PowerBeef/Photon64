"""Enforce complete checkpoint comparisons; never infer pass from log text."""
import json
import sys

def evaluate(text, expected=None):
    decoder = json.JSONDecoder()
    result = None
    for i, char in enumerate(text):
        if char != '{' or (i and text[i - 1] != '\n'):
            continue
        try:
            candidate, end = decoder.raw_decode(text[i:])
            if not text[i + end:].strip():
                result = candidate
                break
        except ValueError:
            pass
    if not isinstance(result, dict):
        raise ValueError('missing or malformed JSON result')
    if result.get('error'):
        raise ValueError(result['error'])
    checks = result.get('checks')
    if not isinstance(checks, list) or not checks:
        raise ValueError('zero checkpoints')
    frames = [c.get('frame') for c in checks]
    if len(set(frames)) != len(frames):
        raise ValueError('duplicate checkpoints')
    if expected is None:
        expected = result.get('expectedChecks')
    if not expected or len(set(expected)) != len(expected) or sorted(frames) != sorted(expected):
        raise ValueError('missing or unexpected checkpoints')
    if result.get('verdict', {}).get('outcome') != 'PASS':
        raise ValueError('runner did not report PASS: ' + str(result.get('verdict')))
    for c in checks:
        if c.get('viDiff') != 0 or c.get('viMax') != 0 or c.get('sync') is not True:
            raise ValueError(f"frame {c.get('frame')}: VI or machine-state mismatch")
        prims = c.get('prims')
        if type(prims) is not int or prims < 0 or (prims > 0 and not c.get('fb')):
            raise ValueError(f"frame {c.get('frame')}: missing rendering coverage")
        if c.get('fb') is not None:
            for kind in ('color', 'depth'):
                value = c['fb'].get(kind, {})
                if value.get('dc') != 0 or value.get('dh') != 0:
                    raise ValueError(f"frame {c.get('frame')}: {kind} or hidden-bit mismatch")
    return result

if __name__ == '__main__':
    try:
        expected = [int(f) for f in sys.argv[2].split(',')] if len(sys.argv) > 2 else None
        r = evaluate(open(sys.argv[1]).read(), expected)
        print(f"PASS: {len(r['checks'])} checkpoints exact")
        print({k: r.get(k) for k in ('compileMs', 'msA', 'msB', 'stats', 'totalMs')})
    except (OSError, ValueError, TypeError, KeyError) as e:
        print('FAIL:', e, file=sys.stderr)
        sys.exit(1)
