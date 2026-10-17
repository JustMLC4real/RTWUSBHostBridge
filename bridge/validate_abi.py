"""Check generated C++ vtable layout against the audited binary's slot contract."""
from pathlib import Path
import json
import re

root = Path(__file__).resolve().parent
text = (root/'compiler_abi.txt').read_text()
expected = json.loads((root/'expected_abi.json').read_text())
checked = []
for cls, data in expected.items():
    start = text.index(f"Vtable for '{cls}'")
    end = text.index('\nVTable indices', start)
    table = text[start:end]
    for slot, declaration in data['methods'].items():
        name = declaration.split('(')[0].split()[-1].lstrip('*')
        entries = re.findall(r'^\s*(\d+) \| .*?'+re.escape(cls+'::'+name)+r'\(', table, re.M)
        if len(entries) != 1:
            raise SystemExit(f'Ambiguous/missing slot: {cls}::{name}: {entries}')
        actual = (int(entries[0])-2)*8
        if actual != int(slot, 16):
            raise SystemExit(f'ABI mismatch: {cls}::{name}: expected {slot}, actual {actual:#x}')
        checked.append({'class':cls, 'method':name, 'slot':slot})
result = {'compiler_layout_matches':True, 'implemented_slot_count':len(checked),
          'checked':checked, 'hardware_deployment_allowed':False,
          'remaining':'Realtek call coverage, lifecycle and power tests; this check proves slot layout only.'}
(root/'ABI_VALIDATED.json').write_text(json.dumps(result,indent=2))
print(f'PASS: {len(checked)} implemented virtual slots have their required legacy offsets')
