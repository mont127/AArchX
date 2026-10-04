actor Counter {
    private var n = 0
    func add(_ k: Int) -> Int { n += k; return n }
    var value: Int { n }
}
func slow(_ i: Int) async -> Int {
    try? await Task.sleep(nanoseconds: UInt64(10 - i) * 1_000_000)
    return i * i
}
@MainActor func onMain() -> String { "main actor ok" }
@main struct M {
    static func main() async {
        let c = Counter()
        await withTaskGroup(of: Void.self) { g in
            for i in 1...100 { g.addTask { _ = await c.add(i) } }
        }
        print("actor", await c.value)
        let squares = await withTaskGroup(of: Int.self) { g -> [Int] in
            for i in 1...8 { g.addTask { await slow(i) } }
            var out: [Int] = []
            for await v in g { out.append(v) }
            return out.sorted()
        }
        print("group", squares)
        async let a = slow(3)
        async let b = slow(4)
        print("async let", await a + b)
        let t = Task.detached { () -> Int in await slow(5) }
        print("detached", await t.value)
        print(await onMain())
        let long = Task { () -> String in
            do { try await Task.sleep(nanoseconds: 5_000_000_000); return "finished" } catch { return "cancelled" }
        }
        long.cancel()
        print("cancel", await long.value)
        let stream = AsyncStream<Int> { cont in
            Task { for i in 1...5 { cont.yield(i) }; cont.finish() }
        }
        var sum = 0
        for await v in stream { sum += v }
        print("stream", sum)
    }
}
