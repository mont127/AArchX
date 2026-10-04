final class Log { static var lines: [String] = [] }

class Animal: CustomStringConvertible {
    let name: String
    init(name: String) { self.name = name }
    func speak() -> String { "..." }
    var description: String { "\(type(of: self))(\(name))" }
    deinit { Log.lines.append("deinit \(name)") }
}
final class Dog: Animal { override func speak() -> String { "woof" } }
final class Cat: Animal { override func speak() -> String { "meow" } }

final class Box<T> {
    var value: T
    init(_ v: T) { value = v }
    func map<U>(_ f: (T) -> U) -> Box<U> { Box<U>(f(value)) }
}

struct Stack<Element> {
    private var items: [Element] = []
    mutating func push(_ e: Element) { items.append(e) }
    mutating func pop() -> Element? { items.popLast() }
    var count: Int { items.count }
}

protocol Shape { var area: Double { get } }
struct Rect: Shape, Hashable { var w: Double, h: Double; var area: Double { w * h } }
struct Circle: Shape { var r: Double; var area: Double { 3.0 * r * r } }

protocol Container {
    associatedtype Item
    var items: [Item] { get }
}
struct Bag: Container { var items: [Int] }
func total<C: Container>(_ c: C) -> Int where C.Item == Int { c.items.reduce(0, +) }

enum Token: Equatable { case num(Int), op(Character), ident(String) }
func tokenize(_ s: String) -> [Token] {
    var out: [Token] = []
    var num = ""
    for c in s {
        if c.isNumber { num.append(c); continue }
        if !num.isEmpty { out.append(.num(Int(num)!)); num = "" }
        if "+-*/".contains(c) { out.append(.op(c)) } else if c.isLetter { out.append(.ident(String(c))) }
    }
    if !num.isEmpty { out.append(.num(Int(num)!)) }
    return out
}

enum ParseError: Error, CustomStringConvertible {
    case empty, bad(String)
    var description: String {
        switch self {
        case .empty: return "empty"
        case .bad(let s): return "bad(\(s))"
        }
    }
}
func parse(_ s: String) throws -> Int {
    if s.isEmpty { throw ParseError.empty }
    guard let v = Int(s) else { throw ParseError.bad(s) }
    return v
}

struct Point { var x: Int; var y: Double; var tag: String? }

final class Node {
    weak var parent: Node?
    var children: [Node] = []
    let id: Int
    init(_ id: Int) { self.id = id }
    deinit { Log.lines.append("node \(id)") }
}

func makeCounter() -> () -> Int {
    var n = 0
    return { n += 1; return n }
}

do {
    let zoo: [Animal] = [Dog(name: "rex"), Cat(name: "tom"), Animal(name: "generic")]
    print(zoo.map { "\($0): \($0.speak())" }.joined(separator: ", "))
    print("dogs", zoo.compactMap { $0 as? Dog }.count, "cats", zoo.filter { $0 is Cat }.count)
    let anys: [AnyObject] = zoo
    print("as AnyObject", anys.count, (anys[1] as? Animal)?.name ?? "nil")
}
print(Log.lines.joined(separator: " | "))
Log.lines.removeAll()

let b = Box(21).map { $0 * 2 }.map { "v=\($0)" }
print(b.value)
var st = Stack<String>()
for w in ["a", "b", "c"] { st.push(w) }
print(st.pop() ?? "-", st.count)

let shapes: [any Shape] = [Rect(w: 2, h: 3), Circle(r: 1), Rect(w: 1.5, h: 4)]
print(shapes.map { $0.area }, shapes.reduce(0.0) { $0 + $1.area })
print(Set([Rect(w: 1, h: 1), Rect(w: 1, h: 1), Rect(w: 2, h: 1)]).count)
print(total(Bag(items: [1, 2, 3, 4])))

print(tokenize("12+x*345-y"))
print(tokenize("1+2") == [.num(1), .op("+"), .num(2)])

for s in ["42", "", "4x2"] {
    do { print("parsed", try parse(s)) } catch { print("error", error) }
}
let r: Result<Int, ParseError> = Result { try parse("7") }.mapError { $0 as! ParseError }
print(r)

let p = Point(x: 1, y: 2.5, tag: "t")
print(p, Optional(Point(x: 3, y: 0, tag: nil)) as Any)
let m = Mirror(reflecting: p)
print(m.children.map { "\($0.label ?? "?")" }.joined(separator: ","))
let kp = \Point.y
print(p[keyPath: kp])

var dict: [String: Int] = [:]
for w in "the quick brown fox jumps over the lazy dog the end".split(separator: " ") { dict[String(w), default: 0] += 1 }
print(dict.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }.joined(separator: " "))

let s = "héllo, wörld 👋🏽"
print(s.count, s.unicodeScalars.count, s.utf8.count, s.uppercased(), String(s.reversed()))
print(s.split(separator: ",").map { $0.trimmingPrefix(" ") })
if let re = try? Regex("w(.)rld"), let match = s.firstMatch(of: re), let g = match.output[1].substring {
    print("regex", g)
}

let c = makeCounter()
_ = c(); _ = c()
print("counter", c())
print([5, 3, 9, 1].sorted(), [5, 3, 9, 1].sorted(by: >), (1...10).filter { $0 % 3 == 0 })
print(Int.max &+ 1 == Int.min, 7 / 2, -7 % 3, 1.0 / 3.0, Double.pi, Float(1) / 3)
print(String(255, radix: 16), Int("ff", radix: 16)!, UInt8(truncatingIfNeeded: 300))

do {
    let root = Node(0)
    let kid = Node(1)
    kid.parent = root
    root.children.append(kid)
    print("parent", kid.parent?.id ?? -1)
    weak var gone: Node?
    do {
        let tmp = Node(2)
        gone = tmp
        print("weak alive", gone?.id ?? -1)
    }
    print("weak after", gone?.id ?? -1)
}
print(Log.lines.joined(separator: " | "))
print("done")
