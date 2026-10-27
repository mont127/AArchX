/*
 * Swift tasks waited for from synchronous code.  Swift concurrency's global
 * executor runs on libdispatch's cooperative pool, whose threads libdispatch
 * asks for with workq_kernreturn(WQOPS_QUEUE_REQTHREADS2) and a cooperative
 * priority.  ocerz dropped that request, so no task ever ran: AXCoreUtilities
 * blocks on a semaphore for one (axUnsafeForcedSync) inside
 * NSSpeechSynthesizer's init, which hung Hearts of Iron IV, and archiveutil
 * waited forever.  Each round starts a detached task that fans out into a
 * task group and blocks until it signals.
 */
import Foundation
import Dispatch

final class Box: @unchecked Sendable { var v = 0 }

func forcedSync(_ body: @escaping @Sendable () async -> Int) -> Int {
    let sem = DispatchSemaphore(value: 0)
    let box = Box()
    Task.detached { box.v = await body(); sem.signal() }
    if sem.wait(timeout: .now() + 10) == .timedOut {
        print("timed out: the task never ran")
        exit(1)
    }
    return box.v
}

func squares(_ n: Int) async -> Int {
    await withTaskGroup(of: Int.self) { g in
        for i in 0..<n { g.addTask { i * i } }
        var s = 0
        for await v in g { s += v }
        return s
    }
}

var r = 0
for _ in 0..<20 { r = forcedSync { await squares(50) } }
print("sum", r)
