import AppKit
import ApplicationServices
import Foundation

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

func attribute(_ element: AXUIElement, _ name: String) -> CFTypeRef? {
    var value: CFTypeRef?
    return AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success ? value : nil
}

guard AXIsProcessTrusted() else {
    fail("macOS 未授权此进程读取 Antigravity 辅助功能数据")
}
guard let app = NSRunningApplication.runningApplications(withBundleIdentifier: "com.google.antigravity").first else {
    fail("Antigravity 未运行")
}

let root = AXUIElementCreateApplication(app.processIdentifier)
AXUIElementSetMessagingTimeout(root, 1)
var stack = [root]
var insideGemini = false
var rows = [[String]]()
var visited = 0
while let element = stack.popLast(), visited < 15_000 {
    visited += 1
    let role = attribute(element, kAXRoleAttribute) as? String
    if role == "AXHeading" || role == "AXStaticText" {
        let text = [kAXTitleAttribute, kAXValueAttribute, kAXDescriptionAttribute]
            .compactMap { attribute(element, $0) as? String }
            .joined(separator: " ")
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed == "Gemini Models" {
            insideGemini = true
        } else if insideGemini && role == "AXHeading" && !trimmed.isEmpty {
            break
        } else if insideGemini && (trimmed.contains("Weekly Limit Remaining") || trimmed.contains("Five Hour Limit Remaining")) {
            rows.append([trimmed])
        } else if insideGemini && !rows.isEmpty && !trimmed.isEmpty {
            if !rows[rows.count - 1].contains(trimmed) {
                rows[rows.count - 1].append(trimmed)
            }
        }
    }
    if let children = attribute(element, kAXChildrenAttribute) as? [AXUIElement] {
        stack.append(contentsOf: children.reversed())
    }
}

guard !rows.isEmpty else {
    fail("未读到 Gemini Models，请保持 Antigravity 的 Models & Usage 面板打开")
}
for row in rows { print(row.joined(separator: " ")) }
