import re
import pprint
import random # XXX remove

def parse(lines):
    regfile = {}
    rx = r' +\( ?([A-Z0-9]+): ([0-9a-f]+)\)'
    for line in lines:
        if not line: break
        for match in re.finditer(rx, line):
            reg, val = match.groups()
            regfile[reg] = int(val, 16)
    return regfile

def load():
    pass

def score(orig, relaxed):
    def _retval(regs):
        return (regs['R14'] << 16) | regs['R15']
    with open(orig, 'r') as origf:
        regs_orig = parse(origf.readlines())
        score_orig = _retval(regs_orig)
    with open(relaxed, 'r') as relaxedf:
        regs_relaxed = parse(relaxedf.readlines())
        score_relaxed = _retval(regs_relaxed)

    # XXX scores represent cycle counts.  dump relevant portions of memory and
    # compare contents instead.
    return abs(score_orig - score_relaxed)

if __name__ == '__main__':
    test_input = \
"""     ( PC: 0ffff)  ( R4: 0ffff)  ( R8: 0ffff)  (R12: 0ffff)
    ( SP: 0ffff)  ( R5: 0ffff)  ( R9: 0ffff)  (R13: 0ffff)
    ( SR: 0ffff)  ( R6: 0ffff)  (R10: 0ffff)  (R14: 0ffff)
    ( R3: 0ffff)  ( R7: 0ffff)  (R11: 0ffff)  (R15: 0ffff)
"""
    pprint.pprint(parse(test_input.split('\n')))