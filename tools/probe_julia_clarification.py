"""Compare extra Julia questions on a small development corpus using pinned local weights."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import types
import hashlib
import time

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model-dir', type=Path, required=True)
    parser.add_argument('--device', default='cuda')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline-ref', default='1133aa5')
    args = parser.parse_args()
    os.environ['JULIA_DEVICE'] = args.device
    spec = importlib.util.spec_from_file_location('julia_probe_worker', ROOT / 'app/runtime/julia_worker.py')
    worker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(worker)
    baseline_source = subprocess.run(['git','show',args.baseline_ref+':app/runtime/julia_worker.py'],
        cwd=ROOT,check=True,capture_output=True).stdout
    baseline = types.ModuleType('julia_baseline_worker')
    exec(compile(baseline_source, '<baseline-julia-worker>', 'exec'), baseline.__dict__)
    engine = worker.load(args.model_dir)
    fixtures = json.loads((ROOT / 'tests/analysis/julia_clarification_development.json').read_text())
    reports = []
    for case in fixtures['cases']:
        row = dict(case)
        for name, module in [('baseline', baseline), ('focused', worker)]:
            start = time.perf_counter()
            result = module.triage(engine, {'triage_plan_version': 2, 'prompt':case['prompt'], 'extended_heads':True})
            scores = {item['category']:item['score'] for item in result['category_scores']}
            clarify = any(scores[key] >= (0.60 if key == 'RULE_CONFLICT' else 0.70) for key in scores)
            row[name] = {'scores':scores, 'atomic':result['atomic_scores'], 'clarify':clarify,
                         'correct':clarify == case['clarify'], 'ms':(time.perf_counter()-start)*1000}
        reports.append(row)
        print(case['id'], row['baseline']['correct'], row['focused']['correct'], flush=True)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({'development_only':True,'baseline_ref':args.baseline_ref,
            'baseline_sha256':hashlib.sha256(baseline_source).hexdigest(),
            'worker_sha256':hashlib.sha256((ROOT/'app/runtime/julia_worker.py').read_bytes()).hexdigest(),
            'weights_sha256':worker.WEIGHTS_SHA256,'runtime':engine.roclint_runtime,
            'goal_question':worker.TRIAGE_GOAL_HEAD,'results':reports},indent=2)+'\n')


if __name__ == '__main__':
    main()
