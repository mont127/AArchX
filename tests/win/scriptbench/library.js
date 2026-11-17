// strings, regular expressions, maths, arrays and dates: the library side of a script engine
function run(rounds) {
    var r, i, acc = 0, words = [], s, parts, m, d;
    for (i = 0; i < 400; i++) words[i] = "word" + (i * 37 % 101) + "_" + i;
    for (r = 0; r < rounds; r++) {
        s = words.join(",");
        parts = s.split(",");
        acc += parts.length + s.indexOf("word50_") + s.lastIndexOf("_3");
        s = s.toUpperCase().substring(10, 900).replace(/WORD(\d+)_/g, "$1:");
        acc += s.length + s.charCodeAt(r % 100);
        m = s.match(/\d+:\d+/g);
        acc += m ? m.length : 0;
        for (i = 0; i < 300; i++) acc += Math.floor(Math.sqrt(i * 3.7) * 10 + Math.sin(i) * 5 + Math.pow(i, 0.5) + Math.abs(Math.cos(i * 0.1)) * 3);
        parts.sort();
        parts.reverse();
        acc += parts.slice(5, 50).concat(parts.slice(100, 120)).length;
        acc += parseInt("" + (r * 12345 % 9973), 10) + parseFloat("3.25e2") + (r * 1.5).toFixed(2).length;
        d = new Date(2026, r % 12, 1 + r % 28);
        acc += d.getDay() + d.getTime() % 7;
    }
    return Math.floor(acc);
}
var n = WScript.Arguments.length ? parseInt(WScript.Arguments(0)) : 60;
var t0 = new Date().getTime();
var res = run(n);
WScript.Echo(res + " " + (new Date().getTime() - t0) + " ms");
