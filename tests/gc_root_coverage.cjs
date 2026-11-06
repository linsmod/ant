// GC root coverage for callback/accumulator paths that are not covered by
// tests/array_callback_gc.cjs.
//
// Every callback below allocates. Under a verify build:
//   ANT_GC_VERIFY=1 ANT_GC_STRESS=1 ./build/ant tests/gc_root_coverage.cjs
// a missing root shows up as a crash or a corrupted result.

function check(name, cond) {
  if (!cond) throw new Error(name);
}

function churn() {
  var keep = [];
  for (var i = 0; i < 16; i++) keep.push({ i: i, s: 'x' + i });
  return keep;
}

function json(v) {
  return JSON.stringify(v);
}

// Array.reduceRight ---------------------------------------------------------
var nums = [1, 2, 3, 4];
check('reduceRight',
  nums.reduceRight(function (a, b) { churn(); return a + b; }, 0) === 10);

// Array.from output array ---------------------------------------------------
check('Array.from',
  json(Array.from([1, 2, 3], function (x) { churn(); return x * 2; })) === '[2,4,6]');
check('Array.from string',
  json(Array.from('abc', function (c) { churn(); return c.toUpperCase(); })) === '["A","B","C"]');

// Object.groupBy result -----------------------------------------------------
var grouped = Object.groupBy([1, 2, 3, 4], function (x) {
  churn();
  return x % 2 ? 'odd' : 'even';
});
check('Object.groupBy',
  grouped.odd.length === 2 && grouped.even.length === 2);

// Map.groupBy result --------------------------------------------------------
if (typeof Map.groupBy === 'function') {
  var mg = Map.groupBy([1, 2, 3, 4], function (x) { churn(); return x % 2; });
  check('Map.groupBy', mg.get(1).length === 2 && mg.get(0).length === 2);
}

// TypedArray callback/accumulator/output ------------------------------------
var ta = new Uint8Array([3, 1, 2]);
var mapped = ta.map(function (x) { churn(); return x + 1; });
check('TypedArray.map', mapped[0] === 4 && mapped[1] === 2 && mapped[2] === 3);
check('TypedArray.reduce',
  ta.reduce(function (a, b) { churn(); return a + b; }, 0) === 6);
if (typeof ta.toSorted === 'function') {
  var sorted = ta.toSorted(function (a, b) { churn(); return a - b; });
  check('TypedArray.toSorted', sorted[0] === 1 && sorted[1] === 2 && sorted[2] === 3);
}
var filtered = ta.filter(function (x) { churn(); return x > 1; });
check('TypedArray.filter', filtered.length === 2 && filtered[0] === 3 && filtered[1] === 2);

// Iterator helpers ----------------------------------------------------------
var it = [1, 2, 3].values();
if (typeof it.reduce === 'function') {
  check('Iterator.reduce',
    it.reduce(function (a, b) { churn(); return a + b; }, 0) === 6);
}
var it2 = [1, 2, 3].values();
if (typeof it2.toArray === 'function') {
  check('Iterator.toArray', json(it2.toArray()) === '[1,2,3]');
}
var it3 = [1, 2, 3].values();
if (typeof it3.find === 'function') {
  check('Iterator.find',
    it3.find(function (x) { churn(); return x === 2; }) === 2);
}

// RegExp / String replace with a function replacement ----------------------
check('RegExp.replace',
  'aaa'.replace(/a/g, function () { churn(); return 'b'; }) === 'bbb');
check('String.replaceAll',
  'aaa'.replaceAll('a', function () { churn(); return 'b'; }) === 'bbb');
check('String.replace',
  'abc'.replace('b', function () { churn(); return 'X'; }) === 'aXc');

// Map / WeakMap upsert ------------------------------------------------------
if (typeof Map.prototype.upsert === 'function') {
  var m = new Map([['a', 1]]);
  var u = m.upsert('a', function (old) { churn(); return old + 1; },
    function () { churn(); return 1; });
  check('Map.upsert', u === 2 && m.get('a') === 2);
}
if (typeof WeakMap.prototype.upsert === 'function') {
  var wk = {};
  var wm = new WeakMap([[wk, 1]]);
  var wu = wm.upsert(wk, function (old) { churn(); return old + 1; },
    function () { churn(); return 1; });
  check('WeakMap.upsert', wu === 2 && wm.get(wk) === 2);
}

console.log('gc root coverage checks passed');
