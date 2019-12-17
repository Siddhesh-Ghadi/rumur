#!/usr/bin/env python3

import abc
import argparse
import codecs
import enum
import itertools
import multiprocessing
import os
import platform
import re
import shutil
import subprocess as sp
import sys
import tempfile
from typing import Optional, Tuple

CPUS = multiprocessing.cpu_count()

STDOUT_ISATTY = os.isatty(sys.stdout.fileno())

def green():  return '\033[32m' if STDOUT_ISATTY else ''
def red():    return '\033[31m' if STDOUT_ISATTY else ''
def yellow(): return '\033[33m' if STDOUT_ISATTY else ''
def reset():  return '\033[0m'  if STDOUT_ISATTY else ''

def enc(s): return s.encode('utf-8', 'replace')
def dec(s): return s.decode('utf-8', 'replace')

# let the user define a range of tests to run
try:
  MIN_TEST = int(os.environ['MIN_TEST'])
except:
  MIN_TEST = None
try:
  MAX_TEST = int(os.environ['MAX_TEST'])
except:
  MAX_TEST = None
def in_range(index: int) -> bool:
  if MIN_TEST is not None and index < MIN_TEST:
    return False
  if MAX_TEST is not None and index > MAX_TEST:
    return False
  return True

print_lock = multiprocessing.Lock()

def pr(s: str) -> None:
  print_lock.acquire()
  sys.stdout.write(s)
  sys.stdout.flush()
  print_lock.release()

def which(cmd: str) -> Optional[str]:
  try:
    return sp.check_output(['which', cmd], stderr=sp.DEVNULL,
      universal_newlines=True).strip()
  except sp.CalledProcessError:
    return None

# C compiler
CC = os.environ.get('CC', which('cc'))

X86_64 = platform.machine() in ('amd64', 'x86_64')

# initial flags to pass to our C compiler
C_FLAGS = ['-x', 'c', '-std=c11', '-Werror=format', '-Werror=sign-compare',
  '-Werror=type-limits'] + (['-mcx16'] if X86_64 else [])

VERIFIER_RNG = os.path.abspath(os.path.join(os.path.dirname(__file__),
  '../misc/verifier.rng'))

def has_sandbox() -> bool:
  'whether the current platform has sandboxing support for the verifier'

  if platform.system() == 'Darwin':
    return True

  if platform.system() == 'FreeBSD':
    return True

  if platform.system() == 'Linux':
    release = platform.release()
    m = re.match(r'(?P<major>\d+)\.(?P<minor>\d+)\.(?P<patch>\d+)', release)

    assert m is not None, 'unrecognised platform.release() string'

    version = tuple(int(m.group(f)) for f in ('major', 'minor', 'patch'))

    return version >= (3, 5, 0)

  if platform.system() == 'OpenBSD':
    return True

  return False

HAS_SANDBOX = has_sandbox()

def smt_args(bv: bool = False) -> [str]:
  'get SMT arguments for an available solver'

  # preference 1: Z3
  if which('z3') is not None:
    # we leave a blank logic here, as Z3 performs best when not given a logic
    args = ['--smt-path', 'z3', '--smt-arg=-smt2', '--smt-arg=-in']
    if bv:
      args += ['--smt-bitvectors', 'on']
    return args

  # preference 2: CVC4
  if which('cvc4') is not None:
    args = ['--smt-path', 'cvc4', '--smt-arg=--lang=smt2',
      '--smt-arg=--rewrite-divk']
    if bv:
      args += ['--smt-prelude', '(set-logic AUFBV)', '--smt-bitvectors', 'on']
    else:
      args += ['--smt-prelude', '(set-logic AUFLIA)']
    return args

  # otherwise, give up
  return []

SMT_ARGS = smt_args()
SMT_BV_ARGS = smt_args(True)

class TemporaryDirectory(object):
  'an mkdtemp() that cleans up after itself'
  def __init__(self):
    self.tmp = None
  def __enter__(self) -> str:
    self.tmp = tempfile.mkdtemp()
    return self.tmp
  def __exit__(self, *_):
    if self.tmp is not None:
      shutil.rmtree(self.tmp)

def run(args: [str], stdin: Optional[str] = None) -> Tuple[int, str, str]:
  if stdin is not None:
    stdin = enc(stdin)
  p = sp.run(args, stdout=sp.PIPE, stderr=sp.PIPE, input=stdin)
  return p.returncode, dec(p.stdout), dec(p.stderr)

HAS_VALGRIND = which('valgrind') is not None

class Result(abc.ABC): pass
class Skip(Result):
  def __init__(self, reason: str): self.reason = reason
class Fail(Result):
  def __init__(self, output: str): self.output = output

class Test(abc.ABC):
  @abc.abstractmethod
  def description(self) -> str: raise NotImplementedError
  @abc.abstractmethod
  def run(self) -> Optional[Result]: raise NotImplementedError

class Tweakable(Test):
  'a test case that can take extra customisation via comment lines'
  def __init__(self):
    # default options
    self.rumur_flags = []
    self.rumur_exit_code = 0
    self.checker_exit_code = 0
    self.checker_output = None
    self.skip_reason = None
  def apply_options(self, model: str) -> None:
    'check for special lines at the start of current model overriding defaults'
    with open(model, 'rt', encoding='utf-8') as f:
      for line in f:
        m = re.match(r'\s*--\s*(?P<key>[a-zA-Z_]\w*)\s*:(?P<value>.*)$', line)
        if m is None:
          break
        key = m.group('key')
        value = m.group('value').strip()
        setattr(self, key, eval(value))

class Model(Tweakable):
  def __init__(self, model: str, debug: bool, optimised: bool, \
      multithreaded: bool, xml: bool, valgrind: bool):
    super().__init__()
    self.model = model
    self.debug = debug
    self.optimised = optimised
    self.multithreaded = multithreaded
    self.xml = xml
    self.valgrind = valgrind
  def description(self) -> str:
    return f'{"D" if self.debug         else " "}' \
           f'{"O" if self.optimised     else " "}' \
           f'{"M" if self.multithreaded else " "}' \
           f'{"X" if self.xml           else " "}' \
           f'{"V" if self.valgrind      else " "}' \
           f' {os.path.basename(self.model)}'
  def run(self) -> Result:

    self.apply_options(self.model)

    if self.skip_reason is not None: return Skip(self.skip_reason)

    if self.valgrind and not HAS_VALGRIND: return Skip('valgrind unavailable')

    # build up arguments to call rumur
    args = ['rumur', '--output', '/dev/stdout', self.model]              \
      + (['--debug'] if self.debug else [])                              \
      + (['--output-format', 'machine-readable'] if self.xml else [])    \
      + (['--threads', '2'] if self.multithreaded and CPUS == 1 else []) \
      + (['--threads', '1'] if not self.multithreaded else [])           \
      + self.rumur_flags

    if self.valgrind:
      args = ['valgrind', '--leak-check=full', '--show-leak-kinds=all',
        '--error-exitcode=42'] + args

    # call rumur
    ret, stdout, stderr = run(args)
    if self.valgrind:
      if ret == 42:
        return Fail(f'Memory leak:\n{stdout}{stderr}')
    if ret != self.rumur_exit_code:
      return Fail(f'Rumur failed:\n{stdout}{stderr}')

    # if we expected to fail, we are done
    if ret != 0: return

    model_c = stdout

    with TemporaryDirectory() as tmp:

      # build up arguments to call the C compiler
      model_bin = os.path.join(tmp, 'model.exe')
      args = [CC] + C_FLAGS + ['-o', model_bin, '-', '-lpthread']

      # call the C compiler
      ret, stdout, stderr = run(args, model_c)
      if ret != 0:
        return Fail(f'C compilation failed:\n{stdout}{stderr}')

      # now run the model itself
      ret, stdout, stderr = run([model_bin])
      if ret != self.checker_exit_code:
        return Fail(f'Unexpected checker exit status {ret}:\n{stdout}{stderr}')

    # if the test has a stdout expectation, check that now
    if self.checker_output is not None:
      if self.checker_output.search(stdout) is None:
        return Fail( 'Checker output did not match expectation regex:\n'
                    f'{stdout}{stderr}')

    # coarse grained check for whether the model contains a 'put' statement that
    # could screw up XML validation
    with open(self.model, 'rt', encoding='utf-8') as f:
      has_put = re.search(r'\bput\b', f.read()) is not None

    if self.xml and not has_put:

      model_xml = stdout

      if which('xmllint') is None: return Skip('xmllint not available')

      # validate the XML
      args = ['xmllint', '--relaxng', VERIFIER_RNG, '--noout', '-']
      ret, stdout, stderr = run(args, model_xml)
      if ret != 0:
        return Fail( 'Failed to XML-validate machine reachable output:\n'
                    f'{stdout}{stderr}')

class Executable(Test):
  def __init__(self, exe: str):
    self.exe = exe
  def description(self) -> str: return f'----- exec {os.path.basename(self.exe)}'
  def run(self) -> Result:
    ret, stdout, stderr = run(self.exe)
    output = f'{stdout}{stderr}'
    return None                 if ret == 0 else \
           Skip(output.strip()) if ret == 125 else \
           Fail(output)

class ASTDumpTest(Tweakable):
  def __init__(self, model: str, valgrind: bool):
    super().__init__()
    self.model = model
    self.valgrind = valgrind
    self.xml = False # dummy setting that tests might reference
  def description(self) -> str:
    return f'----{"V" if self.valgrind else " "} ' \
           f'rumur-ast-dump {os.path.basename(self.model)}'
  def run(self) -> Result:

    self.apply_options(self.model)

    if self.valgrind and not HAS_VALGRIND: return Skip('valgrind unavailable')

    args = ['rumur-ast-dump', self.model]
    if self.valgrind:
      args = ['valgrind', '--leak-check=full', '--show-leak-kinds=all',
        '--error-exitcode=42'] + args
    ret, stdout, stderr = run(args)
    if self.valgrind:
      if ret == 42:
        return Fail(f'Memory leak:\n{stdout}{stderr}')
      # Remainder of the test is unnecessary, because we will already test this
      # in the version of this test when valgrind=False.
      return None

    # if rumur was expected to reject this model, we allow ast-dump to fail
    if self.rumur_exit_code == 0 and ret != 0:
      return Fail(
        f'Unexpected rumur-ast-dump exit status {ret}:\n{stdout}{stderr}')

    if ret != 0:
      return None

    # ast-dump will have written XML to its stdout
    xmlcontent = stdout

    # See if we have xmllint
    if which('xmllint') is None:
      return Skip('xmllint not available for validation')

    # Validate the XML
    rng = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'misc',
      'ast-dump.rng'))
    ret, stdout, stderr = run(['xmllint', '--relaxng', rng, '--noout', '-'],
      xmlcontent)
    if ret != 0:
      return Fail(f'Failed to validate:\n{stdout}{stderr}')

def check(test: Test) -> int:
  'run a test case and report its result'

  result = test.run()

  if result is None:
    pr(f'{green()}PASS{reset()} {test.description()}\n')
    return 1, 0, 0
  elif isinstance(result, Skip):
    pr(f'{yellow()}SKIP{reset()} {test.description()} [{result.reason}]\n')
    return 0, 1, 0
  else:
    assert isinstance(result, Fail)
    pr(f'{red()}FAIL{reset()} {test.description()}\n{result.output}')
    return 0, 0, 1

def main(args: [str]) -> int:

  # setup stdout to make encoding errors non-fatal
  sys.stdout = codecs.getwriter('utf-8')(sys.stdout.buffer, 'replace')

  parser = argparse.ArgumentParser(description='Rumur test suite')
  parser.add_argument('--jobs', '-j', type=int, help='number of threads to use',
    default=CPUS)
  parser.add_argument('testcase', nargs='*',
    help='specific test case(s) to run')
  options = parser.parse_args(args[1:])

  pool = multiprocessing.Pool(options.jobs)

  index = 1
  tests = []

  # find files in our directory
  root = os.path.dirname(__file__)
  for stem in sorted(os.listdir(root)):
    path = os.path.join(root, stem)

    # skip if this does not match the user’s filter
    if len(options.testcase) > 0 and stem not in options.testcase: continue

    # skip directories
    if os.path.isdir(path): continue

    # skip ourselves
    if path == __file__: continue

    # if this is executable, treat it as a test case
    if os.access(path, os.X_OK):
      if in_range(index):
        tests.append(Executable(path))
      index += 1

    # if this is not a model, skip the remaining generic logic
    if os.path.splitext(path)[-1] != '.m': continue

    for debug, optimised, multithreaded, xml, valgrind \
        in itertools.product((False, True), repeat=5):

      # debug output causes invalid XML, so skip
      if debug and xml: continue

      # Valgrind output causes invalid XML, so skip
      if xml and valgrind: continue

      if in_range(index):
        tests.append(Model(path, debug, optimised, multithreaded, xml, valgrind))
      index += 1

    for valgrind in (False, True):
      if in_range(index):
        tests.append(ASTDumpTest(path, valgrind))
      index += 1

  pr(f'Running {len(tests)} tests using {options.jobs} threads...\n'
      '     +------ debug\n'
      '     |+----- optimised\n'
      '     ||+---- multithreaded\n'
      '     |||+--- XML\n'
      '     ||||+-- Valgrind\n')

  # run all tests in parallel and accumulate the results
  passed, skipped, failed = map(sum, zip(*pool.imap_unordered(check, tests)))

  pr(f'{passed} passed, {skipped} skipped, {failed} failed '
     f'out of {len(tests)} total tests\n')

  return 0 if failed == 0 else 1

if __name__ == '__main__':
  sys.exit(main(sys.argv))