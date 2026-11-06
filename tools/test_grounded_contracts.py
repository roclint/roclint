"""Native production-host grounding -> rewrite contract -> post-generation checks."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import check_granite_runtime as native

TOOL = Path(os.environ.get('ROCLINT_CONTRACTS_TOOL', native.find_binary('roclint_granite_contracts')))


@unittest.skipUnless(TOOL.is_file(), 'native contracts tool is not built')
class GroundedContractsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def prepare(self, **changes):
        fixture = {'id': 'native-contract', 'synthetic': True,
                   'prompt': 'Rename run to step in Engine. Do not change behavior.',
                   'files': {'src/engine.hpp': 'class Engine { public: int run(int count); };\n'}}
        fixture.update(changes)
        path = self.root / 'fixture.json'
        path.write_text(json.dumps(fixture), encoding='utf-8')
        result = subprocess.run([str(TOOL), '--rewrite-fixture', str(path)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        return json.loads(result.stdout)

    def validate(self, contract):
        raw = {**contract, 'response': {'ok': True, 'model_id': 'synthetic-parser-fixture',
            'weights_sha256': 'a' * 64, 'quantization': 'Q8_0',
            'result': {'text': json.dumps({'status': 'rewritten', 'rewritten_prompt': contract['prompt']})}}}
        path = self.root / 'response.json'
        path.write_text(json.dumps({'runs': [{'quantization': 'Q8_0', 'cases': [raw]}]}))
        result = subprocess.run([str(TOOL), '--validate', str(path)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        return json.loads(result.stdout)[0]

    def test_real_host_context_enters_prompt_and_proposal_hash(self):
        contract = self.prepare()
        grounding = contract['grounding']
        self.assertTrue(grounding['available'] and grounding['validated'] and grounding['complete'])
        self.assertEqual(len(grounding['ast_hash']), 64)
        self.assertIn(grounding['xml'], contract['instructions'])
        self.assertTrue(any(e['qualifiedTarget'] == 'Engine::run' and e['status'] == 'VERIFIED'
                            for e in grounding['evidence']))
        result = self.validate(contract)
        self.assertTrue(result['parse_valid'] and result['structurally_valid'] and result['evidence_current'])
        self.assertEqual(result['proposal_ast_hash'], grounding['ast_hash'])

    def test_saved_edit_after_generation_withholds_old_proposal(self):
        contract = self.prepare()
        workspace = Path(contract['_grounding_inputs']['root']).resolve()
        self.assertTrue(workspace.is_relative_to(self.root.resolve()))
        (workspace / 'src/engine.hpp').write_text('class Engine { public: int walk(int count); };\n')
        result = self.validate(contract)
        self.assertFalse(result['evidence_current'])
        self.assertTrue(result['withheld'])
        self.assertFalse(result['proposal_published'])

    def test_incomplete_scan_does_not_supply_ast_authority(self):
        contract = self.prepare(maximum_files=1, files={'src/a.hpp': 'int run();', 'src/b.hpp': 'int other();'})
        self.assertFalse(contract['grounding']['available'])
        self.assertEqual(contract['grounding']['xml'], '')
        self.assertEqual(contract['grounding']['ast_hash'], '')
        self.assertEqual(self.validate(contract)['proposal_ast_hash'], '')

    def test_ambiguous_overloads_remain_nonverified(self):
        contract = self.prepare(prompt='Rename Engine::run to step.', files={
            'src/a.hpp': 'class Engine { public: int run(int count); int run(double value); };'})
        self.assertEqual(contract['grounding']['ast_hash'], '')
        self.assertTrue(any(e['status'] == 'AMBIGUOUS' for e in contract['grounding']['evidence']))
        self.assertFalse(any(e['status'] == 'VERIFIED' for e in contract['grounding']['evidence']))

    def discovery(self):
        return {'status': 'options_generated', 'choice_groups': [{'dimension': 'performance',
            'question': 'What should faster mean?', 'material': True, 'alternatives': [
                {'label': 'Lower latency per call', 'why_material': 'Changes the performance goal', 'evidence': ['faster']},
                {'label': 'Higher throughput', 'why_material': 'Changes the performance goal', 'evidence': ['faster']}]}]}

    def test_explicit_clarification_is_bound_by_production_intent_builder(self):
        contract = self.prepare(prompt='Make run faster in Engine.', discovery_reply=self.discovery(),
                                confirmed_alternatives={'grp_0': 'alt_0_0'})
        self.assertIn('Lower latency per call', contract['instructions'])
        self.assertIn('<decisions>', contract['instructions'])

    def test_keep_original_preserves_open_decision_in_full_rewrite_fixture(self):
        contract = self.prepare(prompt='Make run faster in Engine.', discovery_reply=self.discovery(),
                                keep_original_items=['grp_0'], request_mode='skip_questions')
        self.assertIn("Keep the user", contract['instructions'])
        self.assertNotIn('Higher throughput</answer>', contract['instructions'])

    def test_initial_stale_bytes_withhold_context(self):
        contract = self.prepare(mutate_before_grounding={
            'src/engine.hpp': 'class Engine { public: int walk(int count); };'})
        self.assertFalse(contract['grounding']['validated'])
        self.assertEqual(contract['grounding']['xml'], '')
        self.assertEqual(contract['grounding']['ast_hash'], '')


if __name__ == '__main__':
    unittest.main()
