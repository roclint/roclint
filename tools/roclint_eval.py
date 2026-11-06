"""Synthetic AST/rewrite development evaluations; Phoenix is optional and local only.

Offline runs use only the standard library and existing native tools. Publication
uses the separately installed Phoenix environment. No desktop startup dependency.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import xml.etree.ElementTree as ET

import check_granite_runtime as native

ROOT = Path(__file__).resolve().parents[1]
EVALUATOR_VERSION = 1


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_suffix(path.suffix + '.tmp')
    pending.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    pending.replace(path)


def source_identity():
    def git(*args):
        return subprocess.check_output(['git', *args], cwd=ROOT)
    names = sorted(set(git('ls-files', '--cached', '--others', '--exclude-standard', '-z')
                       .decode('utf-8').strip('\0').split('\0')))
    files = {name: sha256(ROOT / name) for name in names if name and (ROOT / name).is_file()}
    fingerprint = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    return {'head': git('rev-parse', 'HEAD').decode().strip(),
            'dirty': bool(git('status', '--porcelain')), 'source_sha256': fingerprint}


def local_endpoint(value):
    parsed = urllib.parse.urlsplit(value)
    if (parsed.scheme not in ('http', 'https') or
            parsed.hostname not in ('127.0.0.1', 'localhost', '::1') or
            parsed.username or parsed.password or parsed.query or parsed.fragment or
            parsed.path not in ('', '/')):
        raise ValueError('Phoenix must use a loopback base URL without credentials or a path')
    return value.rstrip('/')


def load_cases(path, suite):
    data = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    if data.get('synthetic') is not True or data.get('version') != 1:
        raise ValueError('Only versioned, explicitly synthetic evaluation fixtures are accepted')
    cases = data['cases']
    if not cases or len({case['id'] for case in cases}) != len(cases):
        raise ValueError('Fixture IDs must be unique and nonempty')
    for case in cases:
        if not case.get('id') or not case.get('category') or 'expected' not in case:
            raise ValueError('Fixture annotations are incomplete')
        case['suite'] = suite
    return cases


def checked_json(command, *, payload=None, environment=None, timeout=30):
    result = subprocess.run([str(x) for x in command],
                            input=None if payload is None else json.dumps(payload).encode('utf-8'),
                            capture_output=True, env=environment, timeout=timeout, cwd=ROOT)
    if result.returncode:
        raise RuntimeError((result.stdout + result.stderr).decode('utf-8', errors='replace')[-2000:])
    return json.loads(result.stdout.decode('utf-8'))


def score_ast(case, actual):
    errors = []
    evidence = actual.get('evidence', [])
    if len(evidence) != 1:
        return {'passed': False, 'wrong_verified': any(e.get('status') == 'Verified' for e in evidence),
                'errors': ['expected one resolved intent']}
    observed, expected = evidence[0], case['expected']
    for key in ('status', 'target', 'file', 'matchReason', 'complete', 'validateFileHash'):
        if key in expected and observed.get(key) != expected[key]:
            errors.append(key)
    if expected.get('signatureContains', '') not in observed.get('signature', ''):
        errors.append('signatureContains')
    if observed.get('candidates', 0) < expected.get('minCandidates', 0):
        errors.append('minCandidates')
    xml = actual.get('xml', '')
    if len(xml.encode('utf-8')) > 350:
        errors.append('byte_budget')
    try:
        parsed = ET.fromstring(xml)
        emitted = parsed.findall('intent')
        if len(emitted) != actual.get('rendered_count'):
            errors.append('rendered_subset')
        for item in emitted:
            if item.get('status') == 'VERIFIED':
                identity = item.get('target', '')
                if not identity.replace('...', '').strip():
                    errors.append('verified_identity')
    except ET.ParseError:
        errors.append('xml')
    wrong = observed.get('status') == 'Verified' and any(
        key in errors for key in ('status', 'target', 'file', 'signatureContains'))
    return {'passed': not errors, 'wrong_verified': wrong, 'errors': errors}


def score_rewrite(case, actual):
    reply = actual.get('reply', {})
    text = reply.get('rewritten_prompt', '')
    errors = []
    if reply.get('status') != 'rewritten' or not isinstance(text, str) or not text.strip():
        errors.append('missing_rewrite')
        text = ''
    contract = actual.get('contract', {})
    if not contract.get('parse_valid') or not contract.get('structurally_valid'):
        errors.append('production_contract')
    for literal in case['expected'].get('must_preserve_literals', []):
        if literal not in text:
            errors.append('missing_literal:' + literal)
    for phrase in case['expected'].get('prohibition_patterns', []):
        # Exact prohibitions are smoke assertions, not a general semantic judge.
        import re
        if not re.search(phrase, text, re.IGNORECASE):
            errors.append('missing_prohibition:' + phrase)
    return {'passed': not errors, 'errors': errors,
            'semantic_verdict': 'human_review_required', 'fully_faithful': None}


def summarize(rows, selected_count):
    ast = [r for r in rows if r['suite'] == 'ast']
    rewrite = [r for r in rows if r['suite'] == 'rewrite']
    resolvable = [r for r in ast if r['expected'].get('status') == 'Verified']
    resolved = [r for r in resolvable if r.get('score', {}).get('passed')]
    attempted_resolutions = [r for r in resolvable if any(e.get('status') == 'Verified'
        for e in r.get('actual', {}).get('evidence', []))]
    return {'selected_cases': selected_count, 'recorded_cases': len(rows),
            'missing_cases': selected_count - len(rows),
            'errors': sum(r['status'] == 'error' for r in rows),
            'checks_passed': sum(r.get('score', {}).get('passed', False) for r in rows),
            'ast_cases': len(ast), 'rewrite_cases': len(rewrite),
            'supported_resolvable_cases': len(resolvable),
            'correct_resolutions': len(resolved),
            'resolution_coverage': len(attempted_resolutions) / len(resolvable) if resolvable else None,
            'correct_resolution_rate': len(resolved) / len(resolvable) if resolvable else None,
            'wrong_verified': sum(r.get('score', {}).get('wrong_verified', False) for r in ast),
            'verified_decisions': sum(any(e.get('status') == 'Verified' for e in
                r.get('actual', {}).get('evidence', [])) for r in ast),
            'rewrite_human_review_pending': len(rewrite),
            'fully_faithful_rate': None}


def check_annotation_readback(record, row, suite):
    annotations = {item['name']: item for item in record.get('annotations', [])}
    if annotations.get('deterministic_checks', {}).get('score') != float(row['score']['passed']):
        raise RuntimeError('Phoenix annotation differs from local score')
    if suite == 'rewrite':
        semantic = annotations.get('semantic_fidelity')
        if (semantic is None or semantic.get('score') is not None or
                semantic.get('label') != 'human_review_required'):
            raise RuntimeError('Phoenix must retain an explicit unscored semantic-review annotation')


def publish(report, endpoint, checkpoint=lambda: None):
    """Publish saved observations through Phoenix REST and OTLP; no inference replay."""
    endpoint = local_endpoint(endpoint)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(path, data=None, content_type='application/json'):
        body = data if isinstance(data, bytes) else None if data is None else json.dumps(data).encode('utf-8')
        req = urllib.request.Request(endpoint + path, data=body,
                                     headers={'Content-Type': content_type})
        with opener.open(req, timeout=10) as response:
            return response.read()

    def api(path, data=None):
        return json.loads(request(path, data))

    def collect(path):
        rows = []
        for _ in range(1000):
            page = api(path)
            rows.extend(page['data'])
            cursor = page.get('next_cursor')
            if not cursor:
                return rows
            base = path.split('&cursor=')[0]
            path = base + ('&' if '?' in base else '?') + urllib.parse.urlencode({'cursor': cursor})
        raise RuntimeError('Phoenix readback did not exhaust pagination')

    request('/healthz')
    # Import only the generated OTLP messages, not Phoenix's notebook/pandas package.
    from opentelemetry.proto.collector.trace.v1.trace_service_pb2 import (
        ExportTraceServiceRequest, ExportTraceServiceResponse)
    previous = report.get('publication', {})
    if previous.get('experiments'):
        report.setdefault('publication_attempts', []).append(previous)
    state = {'status': 'publishing', 'experiments': []}
    report['publication'] = state
    checkpoint()
    for suite in sorted({row['suite'] for row in report['results']}):
        rows = [row for row in report['results'] if row['suite'] == suite]
        inputs = [{'case_id': row['id'], 'category': row['category'], 'synthetic': True,
                   **row.get('input', {})} for row in rows]
        outputs = [row['expected'] for row in rows]
        identity = hashlib.sha256(json.dumps([inputs, outputs], sort_keys=True).encode()).hexdigest()
        name = f"roclint-{suite}-smoke-{identity[:16]}"
        existing = api('/v1/datasets?' + urllib.parse.urlencode({'name': name, 'limit': 2}))['data']
        if existing:
            if len(existing) != 1:
                raise RuntimeError('Dataset name does not identify one dataset')
            dataset_id = existing[0]['id']
        else:
            uploaded = api('/v1/datasets/upload?sync=true', {'name': name, 'action': 'create',
                'description': 'Synthetic component smoke; no held-out or semantic acceptance claim.',
                'inputs': inputs, 'outputs': outputs,
                'metadata': [{'case_id': row['id'], 'split': 'smoke'} for row in rows]})['data']
            dataset_id = uploaded['dataset_id']
        dataset = api(f'/v1/datasets/{dataset_id}/examples')['data']
        examples = {item['input']['case_id']: item for item in dataset['examples']}
        if set(examples) != {row['id'] for row in rows}:
            raise RuntimeError('Dataset case identity differs from local fixtures')
        for row, input_value in zip(rows, inputs):
            example = examples[row['id']]
            if example['input'] != input_value or example['output'] != row['expected']:
                raise RuntimeError('Dataset version contents differ from local fixture annotations')
        experiment = api(f'/v1/datasets/{dataset_id}/experiments', {
            'name': report['run_id'], 'version_id': dataset['version_id'], 'repetitions': 1,
            'metadata': {'source_sha256': report['source']['source_sha256'],
                'head': report['source']['head'], 'evaluator_version': EVALUATOR_VERSION,
                'fixture_sha256': report['fixture_sha256'], 'measurement_scope': 'component_smoke'}})['data']
        publication = {'suite': suite, 'dataset_id': dataset_id,
            'dataset_version_id': dataset['version_id'], 'experiment_id': experiment['id'],
            'run_ids': [], 'url': endpoint + f"/datasets/{dataset_id}/compare?experimentId={experiment['id']}"}
        state['experiments'].append(publication)
        checkpoint()
        export = ExportTraceServiceRequest()
        resource = export.resource_spans.add()
        resource.resource.attributes.add(key='openinference.project.name').value.string_value = 'roclint-evaluation'
        resource.resource.attributes.add(key='service.name').value.string_value = 'roclint-evaluation'
        scope = resource.scope_spans.add()
        scope.scope.name = 'roclint.synthetic-evaluation'
        scope.scope.version = str(EVALUATOR_VERSION)
        trace_ids = []
        for row in rows:
            span = scope.spans.add(trace_id=uuid.uuid4().bytes, span_id=uuid.uuid4().bytes[:8],
                name='roclint.' + suite + '.' + row['id'], kind=1,
                start_time_unix_nano=row['start_ns'], end_time_unix_nano=row['end_ns'])
            span.status.code = 2 if row['status'] == 'error' else 1
            for key, value in {'openinference.span.kind': 'CHAIN', 'roclint.run_id': report['run_id'],
                    'roclint.case_id': row['id'], 'roclint.status': row['status'],
                    'roclint.source_sha256': report['source']['source_sha256']}.items():
                span.attributes.add(key=key).value.string_value = value
            for key, value in row.get('actual', {}).items():
                if isinstance(value, (int, float)) and not isinstance(value, bool):
                    span.attributes.add(key='roclint.' + key).value.double_value = value
            trace_id = span.trace_id.hex()
            trace_ids.append(trace_id)
            stamp = lambda ns: dt.datetime.fromtimestamp(ns / 1e9, dt.timezone.utc).isoformat()
            run = api(f"/v1/experiments/{experiment['id']}/runs", {
                'dataset_example_id': examples[row['id']]['id'], 'repetition_number': 1,
                'output': {'actual': row.get('actual'), 'checks': row.get('score'), 'status': row['status']},
                'start_time': stamp(row['start_ns']), 'end_time': stamp(row['end_ns']),
                'trace_id': trace_id,
                'error': 'Local execution failed; see local report.' if row.get('error') else None})['data']
            publication['run_ids'].append(run['id'])
            now = dt.datetime.now(dt.timezone.utc).isoformat()
            evaluation = {'experiment_run_id': run['id'], 'annotator_kind': 'CODE',
                'start_time': now, 'end_time': now, 'name': 'deterministic_checks',
                'result': {'score': float(row.get('score', {}).get('passed', False)),
                    'label': 'error' if row['status'] == 'error' else 'pass' if row['score']['passed'] else 'fail',
                    'explanation': json.dumps(row.get('score', {}))}}
            api('/v1/experiment_evaluations', evaluation)
            if suite == 'rewrite':
                api('/v1/experiment_evaluations', {**evaluation, 'name': 'semantic_fidelity',
                    'result': {'score': None, 'label': 'human_review_required',
                               'explanation': 'Literal checks do not establish semantic fidelity.'}})
            checkpoint()
        exported = ExportTraceServiceResponse()
        exported.ParseFromString(request('/v1/traces', export.SerializeToString(), 'application/x-protobuf'))
        if exported.partial_success.rejected_spans:
            raise RuntimeError('Phoenix rejected evaluation traces')
        publication.update(traces_exported=True, trace_ids=trace_ids)
        checkpoint()
        restored = collect(f"/v1/experiments/{experiment['id']}/runs?limit=100")
        restored_by_id = {r['dataset_example_id']: r for r in restored}
        if len(restored) != len(rows):
            raise RuntimeError('Phoenix run count differs from local results')
        for row in rows:
            saved = restored_by_id[examples[row['id']]['id']]
            if saved['output']['checks'] != row['score'] or saved['output']['status'] != row['status']:
                raise RuntimeError('Phoenix output/score differs from local results')
        annotated = api(f"/v1/experiments/{experiment['id']}/json")
        for record in annotated:
            row = next(row for row in rows if examples[row['id']]['id'] == record['example_id'])
            check_annotation_readback(record, row, suite)
        if (len(annotated) != len(rows) or {r['example_id'] for r in annotated} !=
                {examples[row['id']]['id'] for row in rows}):
            raise RuntimeError('Phoenix annotation readback incomplete')
        trace_query = urllib.parse.urlencode([('trace_id', value) for value in trace_ids])
        spans = []
        for _ in range(10):
            try:
                spans = collect('/v1/projects/roclint-evaluation/spans?limit=1000&' + trace_query)
            except urllib.error.HTTPError as error:
                # Project creation/trace indexing can lag an accepted OTLP post.
                if error.code != 404:
                    raise
                spans = []
            if len(spans) == len(rows):
                break
            time.sleep(0.5)
        if len(spans) != len(rows):
            raise RuntimeError('Phoenix trace readback incomplete')
        publication.update(readback_runs=len(restored), readback_traces=len(spans),
                           readback_annotations=len(annotated))
        checkpoint()
    state['status'] = 'published'
    return state


def execute(args):
    cases, fixture_hashes = [], {}
    for suite, path in (('ast', args.ast_fixtures), ('rewrite', args.rewrite_fixtures)):
        if args.suite in (suite, 'all'):
            cases.extend(load_cases(path, suite))
            fixture_hashes[suite] = sha256(path)
    if args.limit:
        cases = cases[:args.limit]
    args.output.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    if args.qt_bin:
        environment['PATH'] = str(args.qt_bin) + os.pathsep + environment.get('PATH', '')
    report = {'schema_version': 1, 'run_id': args.output.name,
              'source': source_identity(), 'fixture_sha256': fixture_hashes,
              'evaluator_version': EVALUATOR_VERSION, 'platform': platform.platform(),
              'logical_cpus': os.cpu_count(), 'scope': 'synthetic component smoke; not held-out acceptance',
              'selected_case_ids': [case['suite'] + ':' + case['id'] for case in cases],
              'results': [], 'publication': {'status': 'not_requested'}}
    worker, worker_error = None, None
    try:
        for case in cases:
            row = {'id': case['id'], 'category': case['category'], 'suite': case['suite'],
                   'expected': case['expected'], 'start_ns': time.time_ns(), 'status': 'error',
                   'input': {key: case[key] for key in ('prompt', 'intent', 'activeFile') if key in case}}
            try:
                if case['suite'] == 'ast':
                    actual = checked_json([args.ast_tool], payload=case, environment=environment)
                    row['score'] = score_ast(case, actual)
                else:
                    if worker_error:
                        raise RuntimeError(worker_error)
                    if worker is None:
                        manifest = json.loads(args.config.read_text(encoding='utf-8-sig'))
                        candidate = next(c for c in manifest['candidates'] if c['quantization'] == 'Q8_0')
                        model_hash = sha256(args.model_dir / candidate['filename'])
                        if model_hash != candidate['sha256']:
                            raise RuntimeError('Model hash differs from pinned manifest')
                        report['runtime'] = {'configuration': manifest, 'weights_sha256': model_hash,
                            'tokenizer_container_sha256': model_hash, 'worker_sha256': sha256(args.worker),
                            'contracts_sha256': sha256(args.contracts_tool)}
                        worker = native.Worker(args, manifest, 'Q8_0', environment)
                        report['runtime']['controls'] = [worker.exchange('hello'), worker.exchange('load')]
                        if any(not c['response']['ok'] for c in report['runtime']['controls']):
                            raise RuntimeError('Native worker failed to load')
                    # Model loading/hash verification is separate from case inference latency.
                    row['start_ns'] = time.time_ns()
                    fixture_path = args.output / ('request-' + case['id'] + '.json')
                    write_json(fixture_path, {**case, 'synthetic': True})
                    contract = checked_json([args.contracts_tool, '--rewrite-fixture', fixture_path],
                                            environment=environment)
                    measurement = worker.exchange(contract['op'],
                        {key: contract[key] for key in ('prompt', 'instructions', 'schema')}, contract)
                    if not measurement['response']['ok']:
                        raise RuntimeError(json.dumps(measurement['response'].get('error', {})))
                    raw = {**contract, **measurement}
                    validation_path = args.output / ('validation-' + case['id'] + '.json')
                    write_json(validation_path, {'runs': [{'quantization': 'Q8_0', 'cases': [raw]}]})
                    validation = checked_json([args.contracts_tool, '--validate', validation_path],
                                              environment=environment)[0]
                    result = measurement['response']['result']
                    actual = {'reply': json.loads(result['text']), 'contract': validation,
                              'exchange_ms': measurement['elapsed_ms'],
                              **{k: v for k, v in result.items() if k != 'text'}}
                    row['score'] = score_rewrite(case, actual)
                row.update(actual=actual, status='measured')
            except Exception as error:
                row['error'] = str(error)
                row['score'] = {'passed': False, 'errors': ['execution_error']}
                if case['suite'] == 'rewrite':
                    worker_error = 'Native evaluation unavailable after error: ' + str(error)
            row['end_ns'] = time.time_ns()
            report['results'].append(row)
            report['summary'] = summarize(report['results'], len(cases))
            write_json(args.output / 'report.json', report)
            print(case['suite'], case['id'], row['status'], row['score']['passed'], flush=True)
    finally:
        if worker:
            report['shutdown'] = worker.close()
    report['source_after'] = source_identity()
    report['source_changed'] = report['source_after'] != report['source']
    report['evaluation_status'] = 'pass' if (not report['source_changed'] and
        report['summary']['checks_passed'] == len(cases)) else 'fail'
    if args.publish:
        try:
            report['publication'] = publish(report, args.endpoint,
                lambda: write_json(args.output / 'report.json', report))
        except Exception as error:
            report['publication'] = {**report.get('publication', {}), 'status': 'failed', 'error': str(error)}
    write_json(args.output / 'report.json', report)
    (args.output / 'cases.jsonl').write_text(''.join(json.dumps(row, ensure_ascii=False) + '\n'
        for row in report['results']), encoding='utf-8')
    print(json.dumps(report['summary'], indent=2))
    print('Publication:', report['publication']['status'])
    return int(report['evaluation_status'] != 'pass' or
               (args.strict_publication and report['publication']['status'] != 'published'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', choices=('ast', 'rewrite', 'all'), default='all')
    parser.add_argument('--ast-fixtures', type=Path, default=ROOT / 'tests/storage/ast_grounding_evaluation.json')
    parser.add_argument('--rewrite-fixtures', type=Path, default=ROOT / 'tests/rewrite/rewrite_fidelity_evaluation.json')
    parser.add_argument('--ast-tool', type=Path, default=native.find_binary('roclint_ast_evaluator'))
    parser.add_argument('--worker', type=Path, default=native.find_binary('roclint_granite_worker', 'runtime'))
    parser.add_argument('--contracts-tool', type=Path, default=native.find_binary('roclint_granite_contracts'))
    parser.add_argument('--config', type=Path, default=native.find_config('granite-runtime-config.json'))
    parser.add_argument('--model-dir', type=Path, default=ROOT / 'models/granite-4.2-3b')
    parser.add_argument('--qt-bin', type=Path)
    parser.add_argument('--limit', type=int)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/evaluations' /
                        ('smoke-' + uuid.uuid4().hex[:12]))
    parser.add_argument('--publish', action='store_true')
    parser.add_argument('--strict-publication', action='store_true')
    parser.add_argument('--endpoint', type=local_endpoint, default='http://127.0.0.1:6006')
    parser.add_argument('--publish-existing', type=Path)
    args = parser.parse_args()
    if args.limit is not None and args.limit < 1:
        parser.error('--limit must be positive')
    args.diagnostic_no_deadlines = False
    if args.publish_existing:
        path = args.publish_existing / 'report.json'
        report = json.loads(path.read_text(encoding='utf-8'))
        try:
            report['publication'] = publish(report, args.endpoint, lambda: write_json(path, report))
        except Exception as error:
            report['publication'] = {**report.get('publication', {}), 'status': 'failed', 'error': str(error)}
        write_json(path, report)
        print(json.dumps(report['publication'], indent=2))
        return int(args.strict_publication and report['publication']['status'] != 'published')
    if args.strict_publication and not args.publish:
        parser.error('--strict-publication requires --publish or --publish-existing')
    return execute(args)


if __name__ == '__main__':
    raise SystemExit(main())
