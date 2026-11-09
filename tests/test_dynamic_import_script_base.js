// Dynamic import() from plain script code (no module context): the compiled
// unit has no explicit import callback and often no code unit at all, so the
// base URL must come from the script that contains the call. Relative
// specifiers resolve against that URL - not against the CWD and not with
// ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING.

async function main() {
  const viaEval = await eval("import('./export-test.js')");
  viaEval.hello('eval-base');

  const direct = await import('./export-test.js');
  direct.hello('direct-base');

  console.log('ok');
}

void main();
