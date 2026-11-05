// Callback-builtin GC regression (agave R63).
//
// A builtin's arguments can arrive through the JIT's alloca buffer, which the
// GC does not scan. Any value a builtin keeps across a JS call (the callback,
// the receiver, the running accumulator) must be rooted explicitly, or a
// collection inside the callback frees the very function the loop is using.
//
// Run with a collection at every allocation to make the hole deterministic:
//   ANT_GC_STRESS=1 ant tests/array_callback_gc.cjs
// (without the roots this dies in sv_callback_call / js_func_closure)

function eq(name, got, want) {
  var g = JSON.stringify(got);
  var w = JSON.stringify(want);
  if (g !== w) throw new Error(name + ': got ' + g + ', want ' + w);
}

var a = [1, 2, 3, 4];

eq('map', a.map(function (x) { return x * 2; }), [2, 4, 6, 8]);
eq('filter', a.filter(function (x) { return x % 2 === 0; }), [2, 4]);
eq('reduce', a.reduce(function (s, x) { return s + x; }, 0), 10);
eq('reduce-no-init', a.reduce(function (s, x) { return s + x; }), 10);
eq('every', a.every(function (x) { return x > 0; }), true);
eq('some', a.some(function (x) { return x === 3; }), true);
eq('find', a.find(function (x) { return x > 2; }), 3);
eq('findIndex', a.findIndex(function (x) { return x > 2; }), 2);
eq('findLast', a.findLast(function (x) { return x < 4; }), 3);
eq('findLastIndex', a.findLastIndex(function (x) { return x < 4; }), 2);
eq('flatMap', a.flatMap(function (x) { return [x, x]; }), [1, 1, 2, 2, 3, 3, 4, 4]);
eq('from', Array.from([1, 2], function (x) { return x + 1; }), [2, 3]);
eq('thisArg', [1].map(function (x) { return x + this.k; }, { k: 10 }), [11]);

if (typeof Object.groupBy === 'function') {
  var grouped = Object.groupBy([1, 2, 3, 4], function (x) { return x % 2 ? 'odd' : 'even'; });
  eq('groupBy', [grouped.odd.length, grouped.even.length], [2, 2]);
}

// Allocate inside every callback body: that is what triggers the collection
// while the loop still holds the callback.
var keep = [];
var mapped = a.map(function (x) {
  keep.push({ x: x });
  if (keep.length > 32) keep.length = 0;
  return x + 1;
});
eq('map-under-allocation', mapped, [2, 3, 4, 5]);

var sum = 0;
var m = new Map([['a', 1], ['b', 2]]);
m.forEach(function (v) { keep.push({ v: v }); sum += v; });
eq('map-forEach', sum, 3);

var setSum = 0;
new Set([5, 6]).forEach(function (v) { setSum += v; });
eq('set-forEach', setSum, 11);

// Throwing callbacks must still propagate and leave no dangling root behind.
var threw = false;
try {
  a.map(function () { throw new Error('boom'); });
} catch (e) {
  threw = e.message === 'boom';
}
eq('throw', threw, true);

console.log('array callback GC checks passed');
