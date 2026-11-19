"""Capture synthetic word questions and check answers with production contracts.
Semantic criteria are review notes, not automated quality scores.
"""
import argparse,json,os,subprocess
from pathlib import Path
from types import SimpleNamespace
from raw_discovery import V13Worker,WORKTREE

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--model-dir',required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--qt-bin',required=True);p.add_argument('--thinking',action='store_true');p.add_argument('--temperature',type=float);p.add_argument('--case',action='append');p.add_argument('--questions-only',action='store_true');a=p.parse_args()
 corpus=json.loads((WORKTREE/'tests/fixtures/companion_eval/word_flow_2026-11-16.json').read_text());assert corpus['synthetic'] is True
 env=os.environ.copy();env['PATH']=a.qt_bin+os.pathsep+env.get('PATH','');config=json.loads((WORKTREE/'build/generated/granite-runtime-config.json').read_text(encoding='utf-8-sig'))
 tool=WORKTREE/'build/Release/roclint_granite_contracts.exe';worker_path=WORKTREE/'build/Release/roclint_granite_worker.exe'
 a.out.parent.mkdir(parents=True,exist_ok=True);directory=a.out.parent/(a.out.stem+'-fixtures');directory.mkdir(exist_ok=True)
 report={'scope':corpus['scope'],'thinking_enabled':a.thinking,'diagnostic_temperature':a.temperature,'cases':[]};worker=None
 def save():a.out.write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding='utf-8')
 def export(fixture,suffix):
  path=directory/(fixture['id']+suffix+'.json');path.write_text(json.dumps({**fixture,'synthetic':True}),encoding='utf-8')
  return json.loads(subprocess.check_output([str(tool),'--word-fixture',str(path)],env=env,timeout=20))
 def generate(fixture,suffix):
  contract=export(fixture,suffix);worker.exchange('reset_cache');reply=worker.exchange(contract['op'],{**{k:contract[k] for k in ('prompt','instructions','schema')},'thinking_enabled':a.thinking},contract)
  if not reply['response'].get('ok'):raise RuntimeError(reply['response'])
  text=reply['response']['result']['text'];result={'raw':text,'elapsed_ms':reply['elapsed_ms']}
  try:result['parsed']=export({**fixture,'model_response':text},suffix+'-parsed')
  except Exception as error:result['parse_error']=str(error)
  return result
 try:
  worker=V13Worker(SimpleNamespace(worker=worker_path,model_dir=Path(a.model_dir),temperature=a.temperature,cpu_reference=False,diagnostic_no_deadlines=False),config,'Q8_0',env)
  report['hello']=worker.exchange('hello');report['load']=worker.exchange('load')
  for case in corpus['cases']:
   if a.case and case['id'] not in a.case:continue
   result=generate(case,'');report['cases'].append({**case,**result});save();print(case['id'],json.dumps(result.get('parsed',result),ensure_ascii=False),flush=True)
  if not a.questions_only:
   fixture={'id':'answer_relevance','prompt':'Make the dashboard feel faster. Preserve all existing controls.','selected':'faster','question':'Which interaction should be faster?','check_answer':True}
   report['invalid_answer']=generate({**fixture,'answer':'iondwfwoefneiofneionfeio'},'-invalid')
   report['valid_answer']=generate({**fixture,'answer':'Refreshing the displayed data'},'-valid')
   report['answered_wording']=generate({'id':'answered_wording','prompt':fixture['prompt'],'selected':'faster','approved_decisions':[fixture['question']+' -> Refreshing the displayed data']},'')
   report['answer_checks_passed']=not report['invalid_answer'].get('parsed',{}).get('accepted',True) and report['valid_answer'].get('parsed',{}).get('accepted',False)
  else:report['answer_checks_skipped']=True
 except Exception as error:report['error']=str(error)
 finally:
  if worker:report['shutdown']=worker.close()
  save()
 return int(bool(report.get('error')) or any('parse_error' in c for c in report['cases']) or (not a.questions_only and not report.get('answer_checks_passed')))
if __name__=='__main__':raise SystemExit(main())
