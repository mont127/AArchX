// objects with methods, property access, strings, arrays: what a script engine's interpreter and heap do
function Obj(i) { this.x = i; this.y = i * 2; this.hp = 100; this.name = "o" + i; }
Obj.prototype.upd = function (dt) { this.x += this.y * dt; if (this.x > 1000) this.y = -this.y; this.hp -= 1; return this.x; };
function run(rounds) {
    var list = [], i, r, sum = 0, s = "";
    for (i = 0; i < 2000; i++) list[i] = new Obj(i);
    for (r = 0; r < rounds; r++) {
        for (i = 0; i < list.length; i++) sum += list[i].upd(0.016);
        if ((r & 7) == 0) { s = ""; for (i = 0; i < 200; i++) s += list[i].name.charAt(1); sum += s.length; }
    }
    var a = [];
    for (i = 0; i < 20000; i++) a[i] = (i * 7919) % 10007;
    a.sort(function (p, q) { return p - q; });
    return Math.floor(sum) + " " + a[5000];
}
var n = WScript.Arguments.length ? parseInt(WScript.Arguments(0)) : 60;
var t0 = new Date().getTime();
var res = run(n);
WScript.Echo(res + " " + (new Date().getTime() - t0) + " ms");
