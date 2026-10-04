import Darwin

final class Log { static var lines: [String] = [] }

class Animal {
    let n: Int
    init(_ n: Int) { self.n = n }
    deinit { Log.lines.append("deinit \(n)") }
}
final class Dog: Animal {}

final class Held {
    let n: Int
    init(_ n: Int) { self.n = n }
    deinit { Log.lines.append("held \(n)") }
}

func flush(_ tag: String) {
    print(tag, Log.lines.joined(separator: " "))
    Log.lines.removeAll()
}

@inline(never) func castArray() {
    let z: [Animal] = [Dog(1), Animal(2), Dog(3)]
    print("dogs", z.compactMap { $0 as? Dog }.map { $0.n })
}

@inline(never) func anyObjects() {
    let objs: [AnyObject] = [Animal(4), Dog(5)]
    print("objects", objs.count)
}

@inline(never) func nested() {
    let rows: [[Animal]] = [[Animal(6)], [Dog(7), Animal(8)]]
    print("rows", rows.map { $0.count })
}

typealias PoolPush = @convention(c) () -> UnsafeMutableRawPointer?
typealias PoolPop = @convention(c) (UnsafeMutableRawPointer?) -> Void

@inline(never) func handOff(_ n: Int) {
    _ = Unmanaged.passRetained(Held(n)).autorelease()
}

castArray()
flush("cast")
anyObjects()
flush("anyobject")
nested()
flush("nested")

let objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW)
let push = unsafeBitCast(dlsym(objc, "objc_autoreleasePoolPush"), to: PoolPush.self)
let pop = unsafeBitCast(dlsym(objc, "objc_autoreleasePoolPop"), to: PoolPop.self)
let pool = push()
for i in 1...3 { handOff(i) }
flush("before pop")
pop(pool)
flush("after pop")

var sum = 0
for round in 0..<20 {
    var batch: [Animal] = []
    for i in 0..<500 { batch.append(i % 2 == 0 ? Dog(i) : Animal(i)) }
    sum += batch.compactMap { $0 as? Dog }.count + round
    batch.removeAll()
    Log.lines.removeAll()
}
print("churn", sum)
print("done")
